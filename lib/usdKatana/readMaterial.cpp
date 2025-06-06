// These files began life as part of the main USD distribution
// https://github.com/PixarAnimationStudios/USD.
// In 2019, Foundry and Pixar agreed Foundry should maintain and curate
// these plug-ins, and they moved to
// https://github.com/TheFoundryVisionmongers/KatanaUsdPlugins
// under the same Modified Apache 2.0 license, as shown below.
//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "usdKatana/readMaterial.h"

#include <cctype>
#include <map>
#include <mutex>
#include <stack>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <pxr/pxr.h>

#include <FnPluginManager/FnPluginManager.h>
#include <FnRendererInfo/plugin/RendererInfoBase.h>
#include <FnRendererInfo/suite/FnRendererInfoSuite.h>
#include <FnRendererInfo/FnRendererInfoPluginClient.h>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/layerUtils.h>

#include <pxr/usd/usdGeom/scope.h>

#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/utils.h>

#include <pxr/usd/usdRi/materialAPI.h>
#include <pxr/usd/usdUI/nodeGraphNodeAPI.h>

#include <FnConfig/FnConfig.h>
#include <FnGeolibServices/FnAttributeFunctionUtil.h>
#include <FnLogging/FnLogging.h>
#include <pystring/pystring.h>

#include "usdKatana/attrMap.h"
#include "usdKatana/baseMaterialHelpers.h"
#include "usdKatana/readPrim.h"
#include "usdKatana/utils.h"

PXR_NAMESPACE_OPEN_SCOPE

FnLogSetup("UsdKatanaReadMaterial");

namespace
{
struct ShadingNodeTraversalData
{
    std::string target{};
    bool targetOverwritten{false};
    bool isMaterial{false};
};

std::unordered_map<std::string, std::vector<std::string>> s_shaderIdToRenderTarget;
std::once_flag s_onceFlag;
}  // namespace

static std::string _CreateShadingNode(UsdPrim shadingNode,
                                      double currentTime,
                                      FnKat::GroupBuilder& nodesBuilder,
                                      FnKat::GroupBuilder& interfaceBuilder,
                                      FnKat::GroupBuilder& layoutBuilder,
                                      const ShadingNodeTraversalData& traversalData,
                                      bool flatten);

SdfLayerHandle _FindLayerHandle(const UsdAttribute& attr, const UsdTimeCode& time);

FnKat::Attribute _GetMaterialAttr(const UsdShadeMaterial& materialSchema,
                                  double currentTime,
                                  const std::string& defaultTargetName,
                                  const bool prmanOutputTarget,
                                  bool flatten);

void _UnrollInterfaceFromPrim(const UsdPrim& prim,
                              double currentTime,
                              const std::string& paramPrefix,
                              FnKat::GroupBuilder& materialBuilder,
                              FnKat::GroupBuilder& layoutBuilder,
                              FnKat::GroupBuilder& interfaceBuilder);

void UsdKatanaReadMaterial(const UsdShadeMaterial& material,
                           bool flatten,
                           const UsdKatanaUsdInPrivateData& data,
                           UsdKatanaAttrMap& attrs,
                           const std::string& looksGroupLocation,
                           const std::string& materialDestinationLocation)
{
    UsdPrim prim = material.GetPrim();
    UsdStageRefPtr stage = prim.GetStage();
    SdfPath primPath = prim.GetPath();
    std::string katanaPath = prim.GetName();

    const bool prmanOutputTarget = data.hasOutputTarget("prman");
    std::string targetName = FnConfig::Config::get("DEFAULT_RENDERER");
    // To ensure that the target field on a material node is set to work
    // with the current renderer, we use the config.  In the future we would
    // like to be able to load in multiple supported output shaders for generic
    // materials, but for now we keep it simple and retain the default of prman
    // We may need more info from the USD file to determine which renderer the
    // material was designed for, and therefore what attributes to set.
    if (targetName.empty())
    {
        targetName = "prman";
    }

    // we do this before ReadPrim because ReadPrim calls ReadBlindData
    // (primvars only) which we don't want to stomp here.
    attrs.set("material", _GetMaterialAttr(material, data.GetCurrentTime(),
        targetName, prmanOutputTarget, flatten));

    const std::string& parentPrefix = (looksGroupLocation.empty()) ?
        data.GetUsdInArgs()->GetRootLocationPath() : looksGroupLocation;

    std::string fullKatanaPath =
        !materialDestinationLocation.empty()
            ? materialDestinationLocation
            : UsdKatanaUtils::ConvertUsdMaterialPathToKatLocation(primPath, data);

    if (!fullKatanaPath.empty() &&
            pystring::startswith(fullKatanaPath, parentPrefix)) {
        katanaPath = fullKatanaPath.substr(parentPrefix.size()+1);

        // these paths are relative in katana
        if (!katanaPath.empty() && katanaPath[0] == '/') {
            katanaPath = katanaPath.substr(1);
        }
    }


    attrs.set("material.katanaPath", FnKat::StringAttribute(katanaPath));
    attrs.set("material.usdPrimName", FnKat::StringAttribute(prim.GetName()));

    UsdKatanaReadPrim(material.GetPrim(), data, attrs);

    attrs.set("type", FnKat::StringAttribute("material"));

    // clears out prmanStatements.
    attrs.set("prmanStatements", FnKat::Attribute());
}


////////////////////////////////////////////////////////////////////////
// Protected methods

static std::string _GetRenderTargetFromRenderContext(const TfToken& renderContext)
{
    static const std::unordered_map<TfToken, std::string, TfToken::HashFunctor>
        s_contextToTargetMap{
            {TfToken("ri"), "prman"},
            {TfToken("glslfx"), "usd"},
            {TfToken("arnold"), "arnold"},
            {TfToken("nsi"), "dl"},
        };

    const auto& it = s_contextToTargetMap.find(renderContext);
    if (it == s_contextToTargetMap.end())
    {
        return {};
    }
    return it->second;
}

struct _TerminalInfo
{
    std::string terminalName;
    std::string targetName;
};
static _TerminalInfo _GetKatanaTerminalName(const std::string& terminalName)
{
    if (terminalName.empty())
    {
        return {terminalName, {}};
    }

    size_t offset = 0;
    std::string prefix;
    if (TfStringStartsWith(terminalName, "ri:"))
    {
        // ri:terminalName -> prmanTerminalName.
        offset = strlen("ri:");
        prefix = "prman";
    }
    else if (TfStringStartsWith(terminalName, "glslfx:"))
    {
        // glslfx:terminalName -> usdTerminalName.
        offset = strlen("glslfx:");
        prefix = "usd";
    }
    else if (TfStringStartsWith(terminalName, "arnold:"))
    {
        // arnold:terminalName -> arnoldTerminalName.
        offset = strlen("arnold:");
        prefix = "arnold";
    }
    else if (TfStringStartsWith(terminalName, "nsi:"))
    {
        // nsi:terminalName -> dlTerminalName.
        offset = strlen("nsi:");
        prefix = "dl";
    }
    else
    {
        // terminalName -> usdTerminalName.
        offset = 0;
        prefix = "usd";
    }

    std::string result = prefix + terminalName.substr(offset);
    if (!prefix.empty())
    {
        result[prefix.size()] = static_cast<char>(
            toupper(result[prefix.size()]));
    }

    return {result, prefix};
}

// Helper to revert encodings made in UsdExport/material.py.
static std::string
_DecodeUsdExportPortName(const std::string& portName, bool isTerminal)
{
    if (isTerminal)
    {
        // The "prmanBxdf" terminal was replaced with "prmanSurface".
        if (portName == "prmanSurface")
        {
            return "prmanBxdf";
        }
    }

    // Individual component ports of the form "port.x" were replaced with
    // "port_x".  We assume we'll only ever see components rgbaxyz.
    std::string result = portName;
    if (result.size() > 2)
    {
        char& penultimate = result[result.size() - 2];
        if (penultimate == ':')
        {
            int last = std::tolower(result.back());
            if (strchr("rgbaxyz", last))
            {
                penultimate = '.';
            }
        }
    }

    return result;
}

template <typename UsdShadeT>
static void _ProcessShaderConnections(const UsdPrim& prim,
                                      const UsdShadeT& connection,
                                      const std::string& handle,
                                      double currentTime,
                                      FnKat::GroupBuilder& nodesBuilder,
                                      FnKat::GroupBuilder& paramsBuilder,
                                      FnKat::GroupBuilder& interfaceBuilder,
                                      FnKat::GroupBuilder& layoutBuilder,
                                      FnKat::GroupBuilder& connectionsBuilder,
                                      std::vector<std::string>& connectionsList,
                                      std::vector<std::string>& returnConnectionsList,
                                      ShadingNodeTraversalData& traversalData,
                                      bool flatten)
{
    static_assert(std::is_same<UsdShadeT, UsdShadeInput>::value ||
                  std::is_same<UsdShadeT, UsdShadeOutput>::value,
                  "UsdShadeT must be an input or an output.");

    std::string connectionId = connection.GetBaseName();
    if (traversalData.isMaterial)
    {
        // Connection id is for example `ri:surface` or `nsi:surface` or `surface`.
        const TfTokenVector tokens = SdfPath::TokenizeIdentifierAsTokens(connectionId);
        if (tokens.size() >= 2)
        {
            traversalData.target = _GetRenderTargetFromRenderContext(tokens[0]);
            traversalData.targetOverwritten = true;
        }
        // If we can't find a rendercontext we don't overwrite the target.
        // The method _GetRenderTargetAndShaderType will return the correct target name.
        else
        {
            traversalData.targetOverwritten = false;
        }
    }

    // We do not try to extract presentation metadata from parameters -
    // only material interface attributes should bother recording such.

    // We can have multiple incoming connections, we get a whole set of paths
    SdfPathVector sourcePaths;
    if (UsdShadeConnectableAPI::GetRawConnectedSourcePaths(connection, &sourcePaths))
    {
        bool multipleConnections = sourcePaths.size() > 1;

        // Check the relationship(s) representing this connection to see if
        // the targets come from a base material. If so, ignore them.
        bool createConnections =
            flatten || !UsdShadeConnectableAPI::IsSourceConnectionFromBaseMaterial(connection);

        int connectionIdx = 0;
        for (const SdfPath& sourcePath : sourcePaths)
        {
            // We only care about connections to output properties
            if (!sourcePath.IsPropertyPath())
            {
                continue;
            }

            UsdShadeConnectableAPI source =
                UsdShadeConnectableAPI::Get(prim.GetStage(), sourcePath.GetPrimPath());
            if (!static_cast<bool>(source))
            {
                continue;
            }

            TfToken sourceName;
            UsdShadeAttributeType sourceType;
            // The sourcePrim and sourcePathName relate to the actual shader Node which this
            // connection is connected to, ignoring any ShadingGroups or other non-contributing
            // nodes. This sourcePrim and sourcePathName may be altered by the logic which
            // iterates through parent UsdShadeNodeGraph prims.
            TfToken sourcePathName = sourcePath.GetNameToken();
            std::tie(sourceName, sourceType) = UsdShadeUtils::GetBaseNameAndType(sourcePathName);
            UsdPrim sourcePrim = source.GetPrim();
            if (sourceType != UsdShadeAttributeType::Output)
            {
                if (sourcePrim.IsA<UsdShadeMaterial>())
                {
                    continue;
                }
                if (!sourcePrim.IsA<UsdShadeNodeGraph>() && !sourcePrim.IsA<UsdShadeMaterial>())
                {
                    continue;
                }
            }

            const std::string layoutTargetHandle = _CreateShadingNode(sourcePrim,
                                                                      currentTime,
                                                                      nodesBuilder,
                                                                      interfaceBuilder,
                                                                      layoutBuilder,
                                                                      traversalData,
                                                                      flatten);

            // We want to unravel Shading node connections which go through a ShadingNodeGraph
            // since material.nodes attributes do not retain NodeGraph or UI elements.
            // We want to retrieve the direct connections between valid shaders.
            // We should have also already created a valid shader via the recursion of
            // _CreateShadingNode above before we get to writing connections down.
            std::string nodeGraphPassthroughHandle = layoutTargetHandle;
            // Cache the ShadingNodeGraph source names etc.. since we will still want to use
            // them for layout attributes.
            TfToken nodeGraphDestinationSourceName = sourceName;
            UsdShadeAttributeType nodeGraphDestinationSourceType = sourceType;
            bool invalidLinks = false;
            while (sourcePrim.IsA<UsdShadeNodeGraph>() && !sourcePrim.IsA<UsdShadeMaterial>())
            {
                UsdAttribute nodeGraphConnection;
                UsdShadeNodeGraph sourceShadeNodeGraph = UsdShadeNodeGraph(sourcePrim);
                if (nodeGraphDestinationSourceType == UsdShadeAttributeType::Input)
                {
                    nodeGraphConnection =
                        sourceShadeNodeGraph.GetInput(nodeGraphDestinationSourceName);
                }
                else if (nodeGraphDestinationSourceType == UsdShadeAttributeType::Output)
                {
                    nodeGraphConnection =
                        sourceShadeNodeGraph.GetOutput(nodeGraphDestinationSourceName);
                }
                else
                {
                    break;
                }
                SdfPathVector shadingNodeGroupSourcePaths;
                nodeGraphConnection.GetConnections(&shadingNodeGroupSourcePaths);
                if (shadingNodeGroupSourcePaths.empty())
                {
                    break;
                }
                for (const auto& nodeGraphSourcePath : shadingNodeGroupSourcePaths)
                {
                    UsdShadeConnectableAPI nodeGraphSource = UsdShadeConnectableAPI::Get(
                        sourcePrim.GetStage(), nodeGraphSourcePath.GetPrimPath());
                    if (!nodeGraphSource)
                    {
                        invalidLinks = true;
                    }
                    std::tie(nodeGraphDestinationSourceName, nodeGraphDestinationSourceType) =
                        UsdShadeUtils::GetBaseNameAndType(nodeGraphSourcePath.GetNameToken());
                    sourcePrim = nodeGraphSource.GetPrim();
                    if (!sourcePrim.IsValid())
                    {
                        invalidLinks = true;
                    }
                    nodeGraphPassthroughHandle =
                        UsdKatanaUtils::GenerateShadingNodeHandle(sourcePrim);
                }
                // To prevent us never returning if there's a circular connection or a
                // connection to a ShadingNodeGroup that doesn't connect to another shader.
                if (invalidLinks)
                {
                    createConnections = false;
                    break;
                }
            }

            // These targets are local, so include them.
            if (createConnections)
            {
                auto CreateUnconnectedShadingGroups = [&](UsdPrim& prim)
                {
                    while (!prim.IsA<UsdShadeMaterial>() && prim.IsA<UsdShadeNodeGraph>())
                    {
                        std::string parentTargetHandle =
                            UsdKatanaUtils::GenerateShadingNodeHandle(prim);
                        if (parentTargetHandle != layoutTargetHandle)
                        {
                            UsdShadeNodeGraph sourceNodeGraph = UsdShadeNodeGraph(prim);
                            if (sourceNodeGraph.GetInputs().empty() &&
                                sourceNodeGraph.GetOutputs().empty())
                            {
                                parentTargetHandle = _CreateShadingNode(prim,
                                                                        currentTime,
                                                                        nodesBuilder,
                                                                        interfaceBuilder,
                                                                        layoutBuilder,
                                                                        traversalData,
                                                                        flatten);
                            }
                        }

                        prim = prim.GetParent();
                    }
                };
                // Check to see if this shader or its source shader has a parent which is a
                // UsdShadeNodeGraph (and not a material), if it does, we need to make sure its
                // connections go via this shader.
                UsdPrim sourceParent = sourcePrim.GetParent();
                CreateUnconnectedShadingGroups(sourceParent);
                UsdPrim parentPrim = prim.GetParent();
                CreateUnconnectedShadingGroups(parentPrim);

                // Only assume the connection needs terminal decoding if
                // it is for an invalid node. I.e the NetworkMaterial node.
                bool isTerminal = true;
                const UsdShadeShader shaderSchema = UsdShadeShader(prim);
                const UsdShadeNodeGraph nodeGraphSchema = UsdShadeNodeGraph(prim);
                const UsdShadeMaterial materialSchema = UsdShadeMaterial(prim);
                if (shaderSchema || (nodeGraphSchema && !materialSchema))
                {
                    isTerminal = false;
                }
                std::string connAttrName =
                    isTerminal ? _GetKatanaTerminalName(connectionId).terminalName : connectionId;
                connAttrName = _DecodeUsdExportPortName(connAttrName, isTerminal);

                // In the case of multiple input connections for array
                // types, we append a ":idx" to the name.
                if (multipleConnections)
                {
                    connAttrName += ":" + std::to_string(connectionIdx);
                    connectionIdx++;
                }

                const std::string sourceStrNodes =
                    _DecodeUsdExportPortName(nodeGraphDestinationSourceName.GetString(),
                                             isTerminal) +
                    '@' + nodeGraphPassthroughHandle;
                const std::string sourceStrLayout =
                    _DecodeUsdExportPortName(sourceName.GetString(), isTerminal) + '@' +
                    layoutTargetHandle;

                connectionsBuilder.set(connAttrName, FnKat::StringAttribute(sourceStrNodes));
                if (nodeGraphSchema && !materialSchema)
                {
                    // A ShadingGroup's nodeSpecificAttributes will record the connections
                    // for input and output ports based on the connectionsList and
                    // returnConnectionsList.
                    if (std::is_same<UsdShadeT, UsdShadeOutput>::value)
                    {
                        returnConnectionsList.push_back(connAttrName + ':' + sourceStrLayout);
                    }
                    else if (std::is_same<UsdShadeT, UsdShadeInput>::value)
                    {
                        connectionsList.push_back(connAttrName + ':' + sourceStrLayout);
                    }
                }
                else
                {
                    connectionsList.push_back(connAttrName + ':' + sourceStrLayout);
                }
            }
        }
    }
    else
    {
        // This input may author an opinion which blocks connections (eg, a
        // connection from a base material). A blocked connection manifests
        // as an authored connection, but no connections can be determined.
        UsdAttribute inputAttr = connection.GetAttr();
        bool hasAuthoredConnections = inputAttr.HasAuthoredConnections();
        SdfPathVector conns;
        inputAttr.GetConnections(&conns);

        // Use a NullAttribute to capture the block
        if (hasAuthoredConnections && conns.empty())
        {
            connectionsBuilder.set(connectionId, FnKat::NullAttribute());
        }
    }

    // produce the value here and let katana handle the connection part
    // correctly..
    UsdAttribute attr = connection.GetAttr();
    VtValue vtValue;
    if (!attr.Get(&vtValue, currentTime))
    {
        return;
    }

    // If the attribute value comes from a base material, leave it
    // empty -- we will inherit it from the parent katana material.
    if (flatten || !UsdKatana_IsAttrValFromSiblingBaseMaterial(attr))
    {
        bool isUdim = false;
        if (vtValue.IsHolding<SdfAssetPath>())
        {
            const SdfAssetPath& assetPath(vtValue.UncheckedGet<SdfAssetPath>());
            const std::string& rawPath = assetPath.GetAssetPath();
            size_t udimIdx = rawPath.rfind("<UDIM>");

            if (udimIdx != std::string::npos)
            {
                isUdim = true;
                UsdTimeCode usdTime(currentTime);
                SdfLayerHandle layer = _FindLayerHandle(attr, usdTime);
                if (layer)
                {
                    const std::string path = SdfComputeAssetPathRelativeToLayer(layer, rawPath);
                    vtValue = VtValue(SdfAssetPath(path));
                    paramsBuilder.set(connectionId,
                                      UsdKatanaUtils::ConvertVtValueToKatAttr(vtValue));
                }
            }
        }
        if (!isUdim)
        {
            paramsBuilder.set(connectionId, UsdKatanaUtils::ConvertVtValueToKatAttr(vtValue));
        }
    }
}

// Templated for shaderSchema derivatives.
template <typename T>
static void _GatherShadingParameters(const T& shaderSchema,
                                     const std::string& handle,
                                     double currentTime,
                                     FnKat::GroupBuilder& nodesBuilder,
                                     FnKat::GroupBuilder& paramsBuilder,
                                     FnKat::GroupBuilder& interfaceBuilder,
                                     FnKat::GroupBuilder& layoutBuilder,
                                     FnKat::GroupBuilder& connectionsBuilder,
                                     std::vector<std::string>& connectionsList,
                                     std::vector<std::string>& returnConnectionsList,
                                     ShadingNodeTraversalData& traversalData,
                                     const bool flatten)
{
    UsdPrim prim = shaderSchema.GetPrim();
    std::string primName = prim.GetName();

    std::vector<UsdShadeInput> shaderInputs = shaderSchema.GetInputs();
    TF_FOR_ALL(shaderInputIter, shaderInputs)
    {
        _ProcessShaderConnections(prim,
                                  *shaderInputIter,
                                  handle,
                                  currentTime,
                                  nodesBuilder,
                                  paramsBuilder,
                                  interfaceBuilder,
                                  layoutBuilder,
                                  connectionsBuilder,
                                  connectionsList,
                                  returnConnectionsList,
                                  traversalData,
                                  flatten);
    }
    std::vector<UsdShadeOutput> shaderOutputs = shaderSchema.GetOutputs();
    TF_FOR_ALL(shaderOutputIter, shaderOutputs)
    {
        _ProcessShaderConnections(prim,
                                  *shaderOutputIter,
                                  handle,
                                  currentTime,
                                  nodesBuilder,
                                  paramsBuilder,
                                  interfaceBuilder,
                                  layoutBuilder,
                                  connectionsBuilder,
                                  connectionsList,
                                  returnConnectionsList,
                                  traversalData,
                                  flatten);
    }
}

static void
_ReadLayoutAttrs(const UsdPrim& shadingNode, const std::string& handle,
                 FnKat::GroupBuilder& layoutBuilder)
{
    UsdUINodeGraphNodeAPI nodeApi(shadingNode);

    // Read displayColor.
    UsdAttribute displayColorAttr = nodeApi.GetDisplayColorAttr();
    if (displayColorAttr)
    {
        GfVec3f color;
        if (displayColorAttr.Get(&color))
        {
            float value[3] = { color[0], color[1], color[2] };
            layoutBuilder.set(handle + ".color",
                              FnKat::FloatAttribute(value, 3, 3));
            layoutBuilder.set(handle + ".nodeShapeAttributes.colorr",
                              FnKat::FloatAttribute(value[0]));
            layoutBuilder.set(handle + ".nodeShapeAttributes.colorg",
                              FnKat::FloatAttribute(value[1]));
            layoutBuilder.set(handle + ".nodeShapeAttributes.colorb",
                              FnKat::FloatAttribute(value[2]));
        }
    }

    // Read position.
    UsdAttribute posAttr = nodeApi.GetPosAttr();
    if (posAttr)
    {
        GfVec2f pos;
        if (posAttr.Get(&pos))
        {
            double value[2] = { pos[0], pos[1] };
            layoutBuilder.set(handle + ".position",
                              FnKat::DoubleAttribute(value, 2, 1));
        }
    }

    // Read expansion state.
    UsdAttribute expansionStateAttr = nodeApi.GetExpansionStateAttr();
    if (expansionStateAttr)
    {
        TfToken expansionState;
        if (expansionStateAttr.Get(&expansionState))
        {
            int value = -1;
            if (expansionState == UsdUITokens->closed)
            {
                value = 0;
            }
            else if (expansionState == UsdUITokens->minimized)
            {
                value = 1;
            }
            else if (expansionState == UsdUITokens->open)
            {
                value = 2;
            }

            if (value >= 0)
            {
                float fvalue = static_cast<float>(value);
                layoutBuilder.set(handle + ".viewState",
                                  FnKat::IntAttribute(value));
                layoutBuilder.set(handle + ".nodeShapeAttributes.viewState",
                                  FnKat::FloatAttribute(fvalue));
            }
        }
    }
    // If there is no parent, we just default to "UsdIn". This would signify to NME that
    // there is no parent shading group or otherwise and that these shaders are at the top.
    if (shadingNode.IsA<UsdShadeMaterial>() || shadingNode.GetParent().IsA<UsdShadeMaterial>())
    {
        layoutBuilder.set(handle + ".parent", FnKat::StringAttribute("UsdIn"));
    }
    else
    {
        const std::string parentHandle =
            UsdKatanaUtils::GenerateShadingNodeHandle(shadingNode.GetParent());
        layoutBuilder.set(handle + ".parent", FnKat::StringAttribute(parentHandle));
    }
}

static void _FillShaderIdToRenderTarget(
    std::unordered_map<std::string, std::vector<std::string>>& output)
{
    std::vector<std::string> pluginNames;
    FnPluginManager::PluginManager::getPluginNames("RendererInfoPlugin",
                                                   pluginNames, 2);

    for (const auto& pluginName : pluginNames)
    {
        FnPluginHandle plugin = FnPluginManager::PluginManager::getPlugin(
            pluginName, "RendererInfoPlugin", 2);
        if (!plugin)
        {
            FnLogError("Cannot find renderer info plugin '" << pluginName
                                                            << "'");
            continue;
        }

        const RendererInfoPluginSuite_v2* suite =
            reinterpret_cast<const RendererInfoPluginSuite_v2*>(
                FnPluginManager::PluginManager::getPluginSuite(plugin));
        if (!suite)
        {
            FnLogError("Error getting renderer info plugin API suite.");
            continue;
        }

        FnRendererInfo::FnRendererInfoPlugin* rendererInfoPlugin =
            new FnRendererInfo::FnRendererInfoPlugin(suite);
        const char* pluginCharFilepath =
            FnPluginManager::PluginManager::getPluginPath(plugin);
        const std::string pluginFilepath =
            (pluginCharFilepath ? pluginCharFilepath : "");
        const std::string pluginDir =
            pystring::os::path::dirname(pluginFilepath);
        const std::string rootPath = pluginDir + "/..";
        rendererInfoPlugin->setPluginPath(pluginDir);
        rendererInfoPlugin->setPluginRootPath(rootPath);
        rendererInfoPlugin->setKatanaPath(FnConfig::Config::get("KATANA_ROOT"));
        rendererInfoPlugin->setTmpPath(FnConfig::Config::get("KATANA_TMPDIR"));

        std::string rendererName;
        rendererInfoPlugin->getRegisteredRendererName(rendererName);

        std::vector<std::string> shaderNames;
        const std::vector<std::string> typeTags;
        rendererInfoPlugin->getRendererObjectNames(kFnRendererObjectTypeShader,
                                                   typeTags, shaderNames);
        for (const auto& shaderName : shaderNames)
        {
            // Two different renderers may use the same shader names (for example "bump2d"
            // in both 3Delight and Arnold). At the moment, if the shader in the USD file is not
            // namespaced, the wrong one may be picked. The solution/workflow is not trivial.
            output[shaderName].push_back(rendererName);
        }
    }
}

// Return two strings which are RenderTarget and ShaderType from Shader Prim and
// defaultRenderTarget. RenderTarget is a short form of renderer name used in the attribute
// "material.nodes.<NODE NAME>.target". ShaderType is a shader name used in the attribute
// "material.nodes.<NODE NAME>.shaderType"
std::tuple<std::string, std::string> _GetRenderTargetAndShaderType(
    const UsdShadeShader& shaderPrim,
    const std::string& defaultRenderTarget)
{
    TfToken shaderId;
    if (!shaderPrim.GetShaderId(&shaderId))
    {
        FnLogError("Cannot find shaderId from " << shaderPrim.GetPath().GetString());
        return {{defaultRenderTarget}, {}};
    }

    // Namespaced is the first priority. Currently, only Arnold uses namespaced shader ID such as
    // "arnold:clump". It's uncertain whether other renderers will use it.
    if (TfTokenVector tokens = SdfPath::TokenizeIdentifierAsTokens(shaderId); tokens.size() == 2u)
    {
        const std::string& renderTarget = tokens[0].GetString();
        const std::string& shaderType = tokens[1].GetString();
        return {renderTarget, shaderType};
    }

    // Other threads can only get here once the lucky thread has filled the map.
    std::string renderTarget = defaultRenderTarget;
    std::call_once(s_onceFlag, [&]() { _FillShaderIdToRenderTarget(s_shaderIdToRenderTarget); });
    if (auto it = s_shaderIdToRenderTarget.find(shaderId); it != s_shaderIdToRenderTarget.end())
    {
        const std::vector<std::string>& renderTargets = it->second;
        if (renderTargets.size() >= 1u &&
            std::find(renderTargets.begin(), renderTargets.end(), defaultRenderTarget) ==
                renderTargets.end())
        {
            renderTarget = renderTargets[0];
        }
    }
    return {renderTarget, shaderId};
}

// NOTE: the Ris codepath doesn't use the interfaceBuilder
std::string _CreateShadingNode(UsdPrim shadingNode,
                               double currentTime,
                               FnKat::GroupBuilder& nodesBuilder,
                               FnKat::GroupBuilder& interfaceBuilder,
                               FnKat::GroupBuilder& layoutBuilder,
                               const ShadingNodeTraversalData& traversalData,
                               bool flatten)
{
    std::string handle = UsdKatanaUtils::GenerateShadingNodeHandle(shadingNode);
    if (handle.empty()) {
        return "";
    }

    const UsdShadeNodeGraph shadingGroup = UsdShadeNodeGraph(shadingNode);
    const UsdShadeMaterial materialPrim = UsdShadeMaterial(shadingNode);
    if (shadingGroup && !materialPrim)
    {
        // Check if we know about this node already
        const FnKat::GroupAttribute curLayout = layoutBuilder.build(layoutBuilder.BuildAndRetain);
        if (curLayout.getChildByName(handle).isValid())
        {
            // If so, just return and don't create anything
            return handle;
        }
        // Create an empty group at the handle to prevent infinite recursion
        layoutBuilder.set(handle, FnKat::GroupBuilder().build());
    }
    else
    {
        // Check if we know about this node already
        const FnKat::GroupAttribute curNodes = nodesBuilder.build(nodesBuilder.BuildAndRetain);
        if (curNodes.getChildByName(handle).isValid())
        {
            // If so, just return and don't create anything
            return handle;
        }

        // Create an empty group at the handle to prevent infinite recursion
        nodesBuilder.set(handle, FnKat::GroupBuilder().build());
    }

    FnKat::GroupBuilder shdNodeBuilder;
    UsdShadeShader shaderSchema = UsdShadeShader(shadingNode);

    // We gather shading parameters even if shaderSchema is invalid; we need to
    // get connection attributes for the enclosing network material.  Moreover
    // we need the hierarchical connection list, for the nodes attribute, and
    // the flattened list, for the layout attribute.
    FnKat::GroupBuilder paramsBuilder;
    FnKat::GroupBuilder connectionsBuilder;
    std::vector<std::string> connectionsList;
    std::vector<std::string> returnConnectionsList;
    ShadingNodeTraversalData curStackData{traversalData};
    if (shadingGroup)
    {
        curStackData.isMaterial = bool{materialPrim};
        _GatherShadingParameters(shadingGroup,
                                 handle,
                                 currentTime,
                                 nodesBuilder,
                                 paramsBuilder,
                                 interfaceBuilder,
                                 layoutBuilder,
                                 connectionsBuilder,
                                 connectionsList,
                                 returnConnectionsList,
                                 curStackData,
                                 flatten);
    }
    else
    {
        _GatherShadingParameters(shaderSchema,
                                 handle,
                                 currentTime,
                                 nodesBuilder,
                                 paramsBuilder,
                                 interfaceBuilder,
                                 layoutBuilder,
                                 connectionsBuilder,
                                 connectionsList,
                                 returnConnectionsList,
                                 curStackData,
                                 flatten);
    }

    FnKat::GroupAttribute paramsAttr = paramsBuilder.build();
    if (paramsAttr.getNumberOfChildren() > 0) {
        shdNodeBuilder.set("parameters", paramsAttr);
    }

    FnKat::GroupAttribute connectionsAttr = connectionsBuilder.build();
    if (connectionsAttr.getNumberOfChildren() > 0)
    {
        shdNodeBuilder.set("connections", connectionsAttr);
    }

    if (!connectionsList.empty())
    {
        FnKat::StringAttribute layoutConns(
            connectionsList.data(), connectionsList.size(), 1);
        layoutBuilder.set(handle + ".connections", layoutConns);
    }

    _ReadLayoutAttrs(shadingNode, handle, layoutBuilder);

    std::string shaderType;
    std::string target = traversalData.target;
    if (shaderSchema)
    {
        std::tie(target, shaderType) =
            _GetRenderTargetAndShaderType(shaderSchema, traversalData.target);
        if (traversalData.targetOverwritten)
        {
            target = traversalData.target;
        }
        shdNodeBuilder.set("type", FnKat::StringAttribute(shaderType));

        if (flatten || !UsdKatana_IsPrimDefFromSiblingBaseMaterial(shadingNode))
        {
            shdNodeBuilder.set("name", FnKat::StringAttribute(handle));
            shdNodeBuilder.set("srcName", FnKat::StringAttribute(handle));
            shdNodeBuilder.set("target", FnKat::StringAttribute(target));
        }
    }
    FnKat::GroupAttribute shdNodeAttr = shdNodeBuilder.build();
    // We don't write shading group information in the material.nodes attributes
    if (!shadingGroup)
    {
        nodesBuilder.set(handle, shdNodeAttr);
    }

    // Copy node attributes to layout so NME can create the shading network.
    // Node specific Attrs.
    FnKat::GroupBuilder nodeSpecificBuilder;
    if (shdNodeAttr.getNumberOfChildren() > 0)
    {
        nodeSpecificBuilder.set("name", shdNodeAttr.getChildByName("name"));
        nodeSpecificBuilder.set("shaderType", FnKat::StringAttribute(shaderType));
        nodeSpecificBuilder.set("target", shdNodeAttr.getChildByName("target"));
    }

    // Shading group specific layout attrs.
    if (shadingGroup && !materialPrim)
    {
        if (!returnConnectionsList.empty())
        {
            FnKat::StringAttribute layoutConns(
                returnConnectionsList.data(), returnConnectionsList.size(), 1);
            nodeSpecificBuilder.set("returnConnections", layoutConns);
        }
        auto writeConnectionsToPorts =
            [](const std::vector<std::string>& connections, FnKat::GroupBuilder& ioPortAttrs)
        {
            for (const auto& connection : connections)
            {
                FnKat::GroupBuilder portAttrs;
                const size_t it = connection.find(":");
                if (it != std::string::npos)
                {
                    const std::string portName = connection.substr(0, it);
                    const FnKat::StringAttribute portNameAttr{portName};
                    portAttrs.set("name", portNameAttr);
                    portAttrs.set("displayName", portNameAttr);
                    // We could figure out what the tags are, but we'd need to potentially
                    // search the network to find where these are connected.
                    // This doesn't prevent functionality but does mean the colour of the knots
                    // isn't totally correct.
                    portAttrs.set("tags", FnKat::StringAttribute());
                    ioPortAttrs.set(portName, portAttrs.build());
                }
            }
        };
        FnKat::GroupBuilder inputPortsAttrs;
        writeConnectionsToPorts(connectionsList, inputPortsAttrs);
        nodeSpecificBuilder.set("inputPorts", inputPortsAttrs.build());
        FnKat::GroupBuilder outputPortsAttrs;
        writeConnectionsToPorts(returnConnectionsList, outputPortsAttrs);
        nodeSpecificBuilder.set("outputPorts", outputPortsAttrs.build());
    }

    FnKat::GroupAttribute nodeSpecificAttrs = nodeSpecificBuilder.build();
    if (nodeSpecificAttrs.getNumberOfChildren() > 0)
    {
        layoutBuilder.set(handle + ".nodeSpecificAttributes", nodeSpecificAttrs);
    }

    // Set Katana node type.
    std::string nodeType;
    if (materialPrim)
    {
        nodeType = "NetworkMaterial";
    }
    else if (shaderSchema)
    {
        nodeType = target + "ShadingNode";
        nodeType[0] = static_cast<char>(std::toupper(nodeType[0]));
    }
    else if (shadingGroup)
    {
        // A Material Prim is a type of ShadingGroup. This conditional must come after.
        nodeType = "ShadingGroup";
    }

    layoutBuilder.set(
        handle + ".katanaNodeType", FnKat::StringAttribute(nodeType));

    return handle;
}

// A specialization of _CreateShadingNode() for outputting layout attributes for
// the enclosing NetworkMaterial.
static std::string
_CreateEnclosingNetworkMaterialLayout(
    const UsdPrim& materialPrim,
    double currentTime,
    FnKat::GroupBuilder& nodesBuilder,
    FnKat::GroupBuilder& interfaceBuilder,
    FnKat::GroupBuilder& layoutBuilder,
    const std::string & targetName,
    bool flatten)
{
    ShadingNodeTraversalData data{targetName};
    std::string handle = _CreateShadingNode(
        materialPrim, currentTime, nodesBuilder, interfaceBuilder, layoutBuilder, data, flatten);

    // Remove from material.nodes, as there is no accompanying shading node.
    nodesBuilder.del(handle);

    // We must put the layout attributes are at material.layout.<primname>, else
    // NME will remove them and recreate new attributes.
    const std::string& expectedName = materialPrim.GetName().GetString();
    if (handle != expectedName)
    {
        FnKat::GroupAttribute layoutAttrs = layoutBuilder.build(
            FnKat::GroupBuilder::BuildAndRetain);
        layoutAttrs = layoutAttrs.getChildByName(handle);
        layoutBuilder.del(handle);
        layoutBuilder.set(expectedName, layoutAttrs);
    }

    return handle;
}

FnKat::Attribute _GetMaterialAttr(const UsdShadeMaterial& materialSchema,
                                  double currentTime,
                                  const std::string& defaultTargetName,
                                  const bool prmanOutputTarget,
                                  bool flatten)
{
    flatten |= !UsdKatana_IsPrimDefFromSiblingBaseMaterial(materialSchema.GetPrim());
    UsdPrim materialPrim = materialSchema.GetPrim();

    // TODO: we need a hasA schema
    UsdRiMaterialAPI riMaterialAPI(materialPrim);
    UsdStageWeakPtr stage = materialPrim.GetStage();

    FnKat::GroupBuilder materialBuilder;
    materialBuilder.set("style", FnKat::StringAttribute("network"));
    FnKat::GroupBuilder nodesBuilder;
    FnKat::GroupBuilder interfaceBuilder;
    FnKat::GroupBuilder layoutBuilder;
    FnKat::GroupBuilder terminalsBuilder;

    /////////////////
    // RSL SECTION
    /////////////////

    ShadingNodeTraversalData prmanData{"prman"};
    // look for surface
    UsdShadeShader surfaceShader = riMaterialAPI.GetSurface(/*ignoreBaseMaterial*/ !flatten);
    if (surfaceShader.GetPrim()) {
        std::string handle = _CreateShadingNode(surfaceShader.GetPrim(),
                                                currentTime,
                                                nodesBuilder,
                                                interfaceBuilder,
                                                layoutBuilder,
                                                prmanData,
                                                flatten);

        terminalsBuilder.set("prmanBxdf",
                                FnKat::StringAttribute(handle));
        terminalsBuilder.set("prmanBxdfPort",
                                FnKat::StringAttribute("out"));
    }

    // look for displacement
    UsdShadeShader displacementShader = riMaterialAPI.GetDisplacement(
        /*ignoreBaseMaterial*/ !flatten);
    if (displacementShader.GetPrim()) {
        std::string handle = _CreateShadingNode(displacementShader.GetPrim(),
                                                currentTime,
                                                nodesBuilder,
                                                interfaceBuilder,
                                                layoutBuilder,
                                                prmanData,
                                                flatten);
        terminalsBuilder.set("prmanDisplacement", FnKat::StringAttribute(handle));
    }

    /////////////////
    // RIS SECTION
    /////////////////
    // this does not exclude the rsl part

    // XXX BEGIN This code is in support of Subgraph workflows
    //           and is currently necessary to match equivalent SGG behavior

    // Look for labeled patterns - TODO: replace with UsdShade::ShadingSubgraph
    std::vector<UsdProperty> properties =
        materialPrim.GetPropertiesInNamespace("patternTerminal");
    if (properties.size()) {
        TF_FOR_ALL(propIter, properties) {
            // if (propIter->Is<UsdRelationship>()) {
            UsdRelationship rel = propIter->As<UsdRelationship>();
            if (not rel) {
                continue;
            }

            SdfPathVector targetPaths;
            rel.GetForwardedTargets(&targetPaths);
            if (targetPaths.size() == 0) {
                continue;
            }
            if (targetPaths.size() > 1) {
                FnLogWarn(
                    "Multiple targets for one output port detected on look:" <<
                    materialPrim.GetPath());
            }

            const SdfPath targetPath = targetPaths[0];
            if (not targetPath.IsPropertyPath()) {
                FnLogWarn("Pattern wants a usd property path, not a prim: "
                    << targetPath.GetString());
                continue;
            }

            SdfPath nodePath = targetPath.GetPrimPath();

            if (UsdPrim patternPrim =
                    stage->GetPrimAtPath(nodePath)) {

                std::string propertyName = targetPath.GetName();
                std::string patternPort = propertyName.substr(
                    propertyName.find(':')+1);

                std::string terminalName = rel.GetName();
                terminalName = terminalName.substr(terminalName.find(':')+1);

                std::string handle = _CreateShadingNode(patternPrim,
                                                        currentTime,
                                                        nodesBuilder,
                                                        interfaceBuilder,
                                                        layoutBuilder,
                                                        prmanData,
                                                        flatten);
                terminalsBuilder.set("prmanCustom_" + terminalName, FnKat::StringAttribute(handle));
                terminalsBuilder.set("prmanCustom_"+terminalName+"Port",
                    FnKat::StringAttribute(patternPort));
            }
            else {
                FnLogWarn("Pattern does not exist at "
                            << targetPath.GetString());
            }
        }
    }
    // XXX END

    // with the current implementation of ris, there are
    // no patterns that are unbound or not connected directly
    // to bxdf's.

    // generate interface for materialPrim and also any "contiguous" scopes
    // that are we encounter.
    //
    // XXX: is this behavior unique to katana or do we stick this
    // into the schema?

    std::vector<UsdShadeOutput> materialOutputs = materialSchema.GetOutputs();
    std::string targetName = defaultTargetName;
    for (auto& materialOutput : materialOutputs)
    {
        if (materialOutput.HasConnectedSource())
        {
            const TfToken materialOutTerminalName =
                materialOutput.GetBaseName();
            if (TfStringStartsWith(materialOutTerminalName.GetString(), "ri:"))
            {
                // Skip since we deal with prman shaders above.
                continue;
            }

            auto [katanaTerminalName, terminalTargetName] =
                _GetKatanaTerminalName(materialOutTerminalName.GetString());
            if (katanaTerminalName.empty())
            {
                continue;
            }

            if (terminalTargetName != "usd")
            {
                // If multiple outputs specify different targets, we'll just use the last one
                // encountered. Not ideal.
                targetName = terminalTargetName;
            }

            UsdShadeConnectableAPI materialOutSource;
            TfToken sourceName;
            UsdShadeAttributeType sourceType;
            materialOutput.GetConnectedSource(&materialOutSource, &sourceName,
                                              &sourceType);
            // Get nested name of connected shader with optional delimiter.
            const std::string shaderName =
                UsdKatanaUtils::GenerateShadingNodeHandle(materialOutSource.GetPrim());
            const std::string katanaTerminalPortName =
                katanaTerminalName + "Port";
            terminalsBuilder.set(katanaTerminalName.c_str(), FnKat::StringAttribute(shaderName));
            terminalsBuilder.set(
                katanaTerminalPortName.c_str(),
                FnKat::StringAttribute(sourceName.GetString()));
        }
    }

    _CreateEnclosingNetworkMaterialLayout(
        materialPrim, currentTime, nodesBuilder, interfaceBuilder,
        layoutBuilder, targetName, flatten);

    std::stack<UsdPrim> dfs;
    dfs.push(materialPrim);
    ShadingNodeTraversalData traversalData{targetName};
    while (!dfs.empty()) {
        UsdPrim curr = dfs.top();
        dfs.pop();

        if (!curr) {
            continue;
        }

        std::string paramPrefix;
        if (curr != materialPrim) {
            if (curr.IsA<UsdShadeShader>()) {
                // XXX: Because we're using a lookDerivesFrom
                // relationship instead of a USD composition construct,
                // we'll need to create every shading node instead of
                // relying on traversing the bxdf.
                // We can remove this once the "derives" usd composition
                // works, along with partial composition
                std::string handle = _CreateShadingNode(curr,
                                                        currentTime,
                                                        nodesBuilder,
                                                        interfaceBuilder,
                                                        layoutBuilder,
                                                        traversalData,
                                                        flatten);
            }

            if (!curr.IsA<UsdGeomScope>()) {
                continue;
            }

            paramPrefix = UsdKatanaUtils::GenerateShadingNodeHandle(curr);
        }

        _UnrollInterfaceFromPrim(
            curr, currentTime, paramPrefix, materialBuilder, layoutBuilder, interfaceBuilder);

        TF_FOR_ALL(childIter, curr.GetChildren()) {
            dfs.push(*childIter);
        }
    }

    materialBuilder.set("nodes", nodesBuilder.build());
    materialBuilder.set("terminals", terminalsBuilder.build());
    materialBuilder.set("interface", interfaceBuilder.build());
    materialBuilder.set("layout", layoutBuilder.build());
    materialBuilder.set("info.name",
                        FnKat::StringAttribute(materialPrim.GetName()));
    materialBuilder.set("info.layoutVersion", FnKat::IntAttribute(2));

    FnKat::GroupBuilder statementsBuilder;
    UsdKatanaReadPrimPrmanStatements(materialPrim, currentTime, statementsBuilder,
                                     prmanOutputTarget);
    // Gather prman statements
    FnKat::GroupAttribute statements = statementsBuilder.build();
    if (statements.getNumberOfChildren()) {
        if (prmanOutputTarget)
        {
            materialBuilder.set("underlayAttrs.prmanStatements", statements);
        }
        materialBuilder.set("usd", statements);
    }


    FnAttribute::GroupAttribute localMaterialAttr = materialBuilder.build();

    if (flatten) {
        // check for parent, and compose with it
        // XXX:
        // Eventually, this "derivesFrom" relationship will be
        // a "derives" composition in usd, in which case we'll have to
        // rewrite this to use partial usd composition
        //
        // Note that there are additional workarounds in using the
        // "derivesFrom"/BaseMaterial relationship in the non-op SGG that
        // would need to be replicated here if the USD Material AttributeFn
        // were to use the UsdIn op instead, particularly with respect to
        // the tree structure that the non-op the SGG creates
        // See _ConvertUsdMAterialPathToKatLocation in
        // katanapkg/plugin/sgg/usd/utils.cpp

        if (materialSchema.HasBaseMaterial()) {
            SdfPath baseMaterialPath = materialSchema.GetBaseMaterialPath();
            if (UsdShadeMaterial baseMaterial = UsdShadeMaterial::Get(stage, baseMaterialPath)) {
                // Make a fake context to grab parent data, and recurse on that
                FnKat::GroupAttribute parentMaterial = _GetMaterialAttr(
                    baseMaterial, currentTime, defaultTargetName, prmanOutputTarget, true);
                FnAttribute::GroupBuilder flatMaterialBuilder;
                flatMaterialBuilder.update(parentMaterial);
                flatMaterialBuilder.deepUpdate(localMaterialAttr);
                return flatMaterialBuilder.build();
            }
            else {
                FnLogError(TfStringPrintf("ERROR: Expected UsdShadeMaterial at %s\n",
                        baseMaterialPath.GetText()).c_str());
            }
        }
    }
    return localMaterialAttr;
}

SdfLayerHandle _FindLayerHandle(const UsdAttribute& attr, const UsdTimeCode& time)
{
    for (const auto& spec : attr.GetPropertyStack(time))
    {
        if (spec->HasDefaultValue() ||
            spec->GetLayer()->GetNumTimeSamplesForPath(spec->GetPath()) > 0)
        {
            return spec->GetLayer();
        }
    }
    return TfNullPtr;
}

/* static */
void _UnrollInterfaceFromPrim(const UsdPrim& prim,
                              double currentTime,
                              const std::string& paramPrefix,
                              FnKat::GroupBuilder& materialBuilder,
                              FnKat::GroupBuilder& layoutBuilder,
                              FnKat::GroupBuilder& interfaceBuilder)
{
    UsdStageRefPtr stage = prim.GetStage();

    // TODO: Right now, the exporter doesn't always move thing into
    // the right spot.  for example, we have "Paint_Base_Color" on
    // /PaintedMetal_Material.Paint_Base_Color
    // Which makes it so we can't use the materialSchema.GetInterfaceInputs()
    // (because /PaintedMetal_Material.Paint_Base_Color doesn't have the
    // corresponding "ri" interfaceInput connection).
    //
    // that should really be on
    // /PaintedMetal_Material/Paint_.Base_Color  which does have that
    // connection.
    //
    UsdShadeMaterial materialSchema(prim);
    std::vector<UsdShadeInput> interfaceInputs =
        materialSchema.GetInterfaceInputs();
    UsdShadeNodeGraph::InterfaceInputConsumersMap interfaceInputConsumers =
        materialSchema.ComputeInterfaceInputConsumersMap(
            /*computeTransitiveMapping*/ true);

    TF_FOR_ALL(interfaceInputIter, interfaceInputs) {
        UsdShadeInput interfaceInput = *interfaceInputIter;
        const UsdAttribute& attr = interfaceInput.GetAttr();
        // Skip invalid interface inputs.
        if (!attr) {
            continue;
        }

        const TfToken& paramName = interfaceInput.GetBaseName();
        const std::string renamedParam = paramPrefix + paramName.GetString();

        // handle parameters with values

        VtValue vtValue;

        if (attr.Get(&vtValue, currentTime) && !vtValue.IsEmpty())
        {
            bool isUdim = false;
            if (vtValue.IsHolding<SdfAssetPath>())
            {
                const SdfAssetPath& assetPath(vtValue.UncheckedGet<SdfAssetPath>());
                const std::string& rawPath = assetPath.GetAssetPath();
                size_t udimIdx = rawPath.rfind("<UDIM>");

                if (udimIdx != std::string::npos)
                {
                    isUdim = true;
                    UsdTimeCode usdTime(currentTime);
                    SdfLayerHandle layer = _FindLayerHandle(attr, usdTime);
                    if (layer)
                    {
                        const std::string path = SdfComputeAssetPathRelativeToLayer(layer, rawPath);
                        vtValue = VtValue(SdfAssetPath(path));
                        materialBuilder.set(TfStringPrintf("parameters.%s", renamedParam.c_str()),
                                            UsdKatanaUtils::ConvertVtValueToKatAttr(vtValue));
                    }
                }
            }
            if (!isUdim)
            {
                materialBuilder.set(TfStringPrintf("parameters.%s", renamedParam.c_str()),
                                    UsdKatanaUtils::ConvertVtValueToKatAttr(vtValue));
            }
        }

        if (interfaceInputConsumers.count(interfaceInput) == 0) {
            continue;
        }

        const std::vector<UsdShadeInput> &consumers =
            interfaceInputConsumers.at(interfaceInput);

        for (const UsdShadeInput& consumer : consumers)
        {
            UsdPrim consumerPrim = consumer.GetPrim();

            TfToken inputName = consumer.GetBaseName();

            std::string handle = UsdKatanaUtils::GenerateShadingNodeHandle(consumerPrim);

            std::string srcKey = renamedParam + ".src";

            std::string srcVal = TfStringPrintf(
                    "%s.%s",
                    handle.c_str(),
                    inputName.GetText());

            interfaceBuilder.set(
                    srcKey.c_str(),
                    FnKat::StringAttribute(srcVal),
                    true);

            // Add hints on the interface parameters that the NME needs to set the interface's
            // 'appear as' name.
            const std::string hintsStr = "{'dstName':'" + renamedParam + "'}";
            layoutBuilder.set(
                TfStringPrintf(
                    "%s.parameterVars.%s.hints", handle.c_str(), inputName.GetString().c_str()),
                FnKat::StringAttribute(hintsStr));
            layoutBuilder.set(
                TfStringPrintf(
                    "%s.parameterVars.%s.enable", handle.c_str(), inputName.GetString().c_str()),
                FnKat::IntAttribute(0));
        }

        // USD's group delimiter is :, whereas Katana's is .
        std::string page = TfStringReplace(
                interfaceInput.GetDisplayGroup(), ":", ".");
        if (!page.empty()) {
            std::string pageKey = renamedParam + ".hints.page";
            interfaceBuilder.set(pageKey, FnKat::StringAttribute(page), true);
        }

        std::string doc = interfaceInput.GetDocumentation();
        if (!doc.empty()) {
            std::string docKey = renamedParam + ".hints.help";

            doc = TfStringReplace(doc, "'", "\"");
            doc = TfStringReplace(doc, "\n", "\\n");

            interfaceBuilder.set(docKey, FnKat::StringAttribute(doc), true);
        }
    }
}


PXR_NAMESPACE_CLOSE_SCOPE


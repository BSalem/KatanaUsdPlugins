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
#include <FnGeolibServices/FnBuiltInOpArgsUtil.h>

#include <stdio.h>

#include <memory>
#include <sstream>

#include <pxr/base/tf/pathUtils.h>
#include <pxr/pxr.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/usd/usd/modelAPI.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/motionAPI.h>

#include <FnGeolib/op/FnGeolibOp.h>
#include <FnGeolib/util/Path.h>
#include <FnLogging/FnLogging.h>

#include <pystring/pystring.h>

#include <FnAttributeFunction/plugin/FnAttributeFunctionPlugin.h>

#include <FnAPI/FnAPI.h>

#include "usdKatana/blindDataObject.h"
#include "usdKatana/bootstrap.h"
#include "usdKatana/cache.h"
#include "usdKatana/locks.h"
#include "usdKatana/readBlindData.h"
#include "usdKatana/usdInPluginRegistry.h"
#include "usdKatana/utils.h"
#include "vtKatana/bootstrap.h"

FnLogSetup("UsdIn")

PXR_NAMESPACE_USING_DIRECTIVE

namespace FnKat = Foundry::Katana;

// convenience macro to report an error.
#define ERROR(...)\
    interface.setAttr("type", Foundry::Katana::StringAttribute("error"));\
    interface.setAttr("errorMessage", Foundry::Katana::StringAttribute(\
        TfStringPrintf(__VA_ARGS__)));

static UsdKatanaUsdInArgsRefPtr InitUsdInArgs(const FnKat::GroupAttribute& opArgs,
                                              FnKat::GroupAttribute& additionalOpArgs,
                                              const std::string& rootLocationPath)
{
    ArgsBuilder ab;

    FnKat::StringAttribute usdFileAttr = opArgs.getChildByName("fileName");
    if (!usdFileAttr.isValid())
    {
        return ab.buildWithError("UsdIn: USD fileName not specified.");
    }

    std::string fileName = usdFileAttr.getValue();
        
    ab.rootLocation = FnKat::StringAttribute(
        opArgs.getChildByName("location")).getValue(
            rootLocationPath, false);
        
    std::string sessionLocation = ab.rootLocation;
    FnKat::StringAttribute sessionLocationAttr = 
        opArgs.getChildByName("sessionLocation");
    if (sessionLocationAttr.isValid()) {
        sessionLocation = sessionLocationAttr.getValue();
    }
        
    FnAttribute::GroupAttribute sessionAttr = 
        opArgs.getChildByName("session");
        
    // XXX BEGIN convert the legacy variant string to the session
    // TODO(pxr): decide how long to do this as this form has been deprecated
    //            for some time but may still be present in secondary uses
    FnAttribute::GroupBuilder legacyVariantsGb;
        
    std::string variants = FnKat::StringAttribute(
        opArgs.getChildByName("variants")).getValue("", false);
    std::set<std::string> selStrings = TfStringTokenizeToSet(variants);
    for (const std::string& selString : selStrings)
    {
        std::string errMsg;
        if (SdfPath::IsValidPathString(selString, &errMsg))
        {
            SdfPath varSelPath(selString);
            if (varSelPath.IsPrimVariantSelectionPath())
            {
                std::string entryPath = FnAttribute::DelimiterEncode(
                    sessionLocation + varSelPath.GetPrimPath().GetString());
                std::pair<std::string, std::string> sel =
                    varSelPath.GetVariantSelection();
                    
                legacyVariantsGb.set(entryPath + "." + sel.first,
                                     FnAttribute::StringAttribute(sel.second));
                continue;
            }
        }
            
        return ab.buildWithError(
            TfStringPrintf("UsdIn: Bad variant selection \"%s\"",
                           selString.c_str()).c_str());
    }
        
    FnAttribute::GroupAttribute legacyVariants = legacyVariantsGb.build();
        
    if (legacyVariants.getNumberOfChildren() > 0)
    {
        sessionAttr = FnAttribute::GroupBuilder()
            .set("variants", legacyVariants)
            .deepUpdate(sessionAttr)
            .build();
    }
    // XXX END

    ab.sessionLocation = sessionLocation;
    ab.sessionAttr = sessionAttr;

    ab.ignoreLayerRegex = FnKat::StringAttribute(
        opArgs.getChildByName("ignoreLayerRegex")).getValue("", false);

    ab.verbose = FnKat::IntAttribute(
        opArgs.getChildByName("verbose")).getValue(0, false);

    typedef FnAttribute::StringAttribute::array_type StringArrayType;
    FnAttribute::StringAttribute outputTargetArgStr = opArgs.getChildByName("outputTargets");
    if (outputTargetArgStr.isValid())
    {
        StringArrayType outputTargetVector = outputTargetArgStr.getNearestSample(0);
        for (auto& target : outputTargetVector)
        {
            ab.outputTargets.insert(target);
        }
    }

    FnKat::GroupAttribute systemArgs(opArgs.getChildByName("system"));

    ab.currentTime = 
        FnKat::FloatAttribute(systemArgs.getChildByName(
                                  "timeSlice.currentTime")).getValue(0, false);

    int numSamples = 
        FnKat::IntAttribute(systemArgs.getChildByName(
                                "timeSlice.numSamples")).getValue(1, false);

    ab.shutterOpen =
        FnKat::FloatAttribute(systemArgs.getChildByName(
                                  "timeSlice.shutterOpen")).getValue(0, false);

    ab.shutterClose =
        FnKat::FloatAttribute(systemArgs.getChildByName(
                                  "timeSlice.shutterClose")).getValue(0, false);

    std::string motionSampleStr = FnKat::StringAttribute(
        opArgs.getChildByName("motionSampleTimes")).getValue("", false);

    // If motion samples was specified, convert the string of values
    // into a vector of doubles to store with the root args.
    if (numSamples < 2 || motionSampleStr.empty())
    {
        ab.motionSampleTimes.push_back(0);
    }
    else
    {
        std::vector<std::string> tokens;
        pystring::split(motionSampleStr, tokens, " ");

        for (std::vector<std::string>::iterator it = tokens.begin(); 
             it != tokens.end(); ++it)
        {
            ab.motionSampleTimes.push_back(std::stod(*it));
        }
    }

    // Determine whether to prepopulate the USD stage.
    ab.prePopulate =
        FnKat::IntAttribute(opArgs.getChildByName("prePopulate"))
        .getValue(1 /* default prePopulate=yes */ , false);

    ab.isolatePath = FnKat::StringAttribute(
        opArgs.getChildByName("isolatePath")).getValue("", false);

    {
        const std::string assetResolverContextStr =
            FnKat::StringAttribute(opArgs.getChildByName("assetResolverContext"))
                .getValue("", false);
        const ArResolverContext context =
            assetResolverContextStr.empty()
                ? ArGetResolver().CreateDefaultContextForAsset(fileName)
                : ArGetResolver().CreateContextFromString(assetResolverContextStr);
        const ArResolverContextBinder bind(context);
        ab.stage = UsdKatanaCache::GetInstance().GetStage(fileName,
                                                          sessionAttr,
                                                          sessionLocation,
                                                          ab.isolatePath,
                                                          ab.ignoreLayerRegex,
                                                          ab.prePopulate);
    }

    if (!ab.stage) {
        return ab.buildWithError("UsdIn: USD Stage cannot be loaded.");
    }

    FnAttribute::StringAttribute instanceModeAttr = opArgs.getChildByName("instanceMode");
    if (instanceModeAttr.getValue("expanded", false) == "as sources and instances")
    {
        FnKat::GroupAttribute mappingAttr =
            UsdKatanaUtils::BuildInstancePrototypeMapping(ab.stage, SdfPath::AbsoluteRootPath());
        additionalOpArgs = FnKat::GroupAttribute("prototypeMapping", mappingAttr, true);
    }

    // if the specified isolatePath is not a valid prim, clear it out
    if (!ab.isolatePath.empty() && !ab.stage->GetPrimAtPath(
            SdfPath(ab.isolatePath)))
    {
        std::ostringstream errorBuffer;
        errorBuffer << "UsdIn: Invalid isolatePath: " << 
            ab.isolatePath << ".";
        return ab.buildWithError(errorBuffer.str());
    }

    // get extra attributes or namespaces if they exist
    FnKat::StringAttribute extraAttributesOrNamespacesAttr = 
        opArgs.getChildByName("extraAttributesOrNamespaces");

    if (extraAttributesOrNamespacesAttr.isValid())
    {
        std::vector<std::string> tokens;

        FnKat::StringAttribute::array_type values =
            extraAttributesOrNamespacesAttr.getNearestSample(0.0f);
            
        for (FnKat::StringAttribute::array_type::const_iterator I =
                 values.begin(); I != values.end(); ++I)
        {
            std::string value(*I);
            if (value.empty())
            {
                continue;
            }
                
            pystring::split(value, tokens, ":", 1);
            ab.extraAttributesOrNamespaces[tokens[0]].push_back(value);
        }
    }
        
    FnKat::StringAttribute materialBindingPurposesAttr = 
        opArgs.getChildByName("materialBindingPurposes");
    if (materialBindingPurposesAttr.getNumberOfValues())
    {
        auto sample = materialBindingPurposesAttr.getNearestSample(0.0f);

        for (const auto & v : sample)
        {
            ab.materialBindingPurposes.emplace_back(v);
        }
    }

    // always include userProperties if not explicitly included.
    if (ab.extraAttributesOrNamespaces.find("userProperties")
        == ab.extraAttributesOrNamespaces.end())
    {
        ab.extraAttributesOrNamespaces["userProperties"].push_back(
            "userProperties");
    }
    else
    {
        // if it is there, enforce that it includes only the top-level attr
        std::vector<std::string> & userPropertiesNames =
            ab.extraAttributesOrNamespaces["userProperties"];
            
        userPropertiesNames.clear();
        userPropertiesNames.push_back("userProperties");
    }

    ab.evaluateUsdSkelBindings = static_cast<bool>(
        FnKat::IntAttribute(
            opArgs.getChildByName("evaluateUsdSkelBindings"))
        .getValue(1, false));

    return ab.build();
}


// see overview.dox for more documentation.
class UsdInOp : public FnKat::GeolibOp
{

public:

    static void setup(FnKat::GeolibSetupInterface &interface)
    {
        // Tell katana that it's safe to run this op in a runtime concurrently
        // with other runtimes.
        interface.setThreading(
                FnKat::GeolibSetupInterface::ThreadModeConcurrent);

        _hasSiteKinds = UsdKatanaUsdInPluginRegistry::HasKindsForSite();
    }

    static void flush()
    {
        UsdKatanaCache::GetInstance().Flush();
    }

    static void cook(FnKat::GeolibCookInterface &interface)
    {
        boost::shared_lock<boost::upgrade_mutex> 
            readerLock(UsdKatanaGetStageLock());

        UsdKatanaUsdInPrivateData* privateData =
            static_cast<UsdKatanaUsdInPrivateData*>(interface.getPrivateData());

        // We may be constructing the private data locally -- in which case
        // it will not be destroyed by the Geolib runtime.
        // This won't be used directly but rather just filled if the private
        // needs to be locally built.
        std::unique_ptr<UsdKatanaUsdInPrivateData> localPrivateData;

        FnKat::GroupAttribute opArgs = interface.getOpArg();
        
        // Get usdInArgs.
        UsdKatanaUsdInArgsRefPtr usdInArgs;
        if (privateData) {
            usdInArgs = privateData->GetUsdInArgs();
        } else {
            FnKat::GroupAttribute additionalOpArgs;
            usdInArgs = InitUsdInArgs(interface.getOpArg(), additionalOpArgs,
                    interface.getRootLocationPath());
            opArgs = FnKat::GroupBuilder()
                .update(opArgs)
                .deepUpdate(additionalOpArgs)
                .build();
            
            // Construct local private data if none was provided by the parent.
            // This is a legitmate case for the root of the scene -- most
            // relevant with the isolatePath pointing at a deeper scope which
            // may have meaningful type/kind ops.
            
            if (usdInArgs->GetStage())
            {
                localPrivateData.reset(new UsdKatanaUsdInPrivateData(usdInArgs->GetRootPrim(),
                                                                     usdInArgs, privateData));
                privateData = localPrivateData.get();
            }
            if (FnAttribute::GroupAttribute prototypeMapping =
                    additionalOpArgs.getChildByName("prototypeMapping");
                prototypeMapping.isValid())
            {
                privateData->setInstancePrototypeMapping(prototypeMapping);
            }
        }
        // Validate usdInArgs.
        if (!usdInArgs) {
            ERROR("Could not initialize UsdIn usdInArgs.");
            return;
        }
        
        if (!usdInArgs->GetErrorMessage().empty())
        {
            ERROR("%s", usdInArgs->GetErrorMessage().c_str());
            return;
        }

        UsdStagePtr stage = usdInArgs->GetStage();
        
        // If privateData wasn't initialized because there's no stage in
        // usdInArgs, it would have been caught before as part of the check
        // for usdInArgs->GetErrorMessage(). check again for safety.
        UsdPrim prim;
        if (privateData)
        {
            prim = privateData->GetUsdPrim();
        }
        
        // Validate usd prim.
        if (!prim) {
            ERROR("No USD prim at %s",
                  interface.getRelativeOutputLocationPath().c_str());
            return;
        }

        // Determine if we want to perform the stage-wide queries.
        FnAttribute::IntAttribute processStageWideQueries = 
            opArgs.getChildByName("processStageWideQueries");
        if (processStageWideQueries.isValid() &&
            processStageWideQueries.getValue(0, false) == 1) {
            interface.stopChildTraversal();
            // Reset processStageWideQueries for children ops.
            opArgs = FnKat::GroupBuilder()
                .update(opArgs)
                .set("processStageWideQueries", FnAttribute::IntAttribute(0))
                .build();

            const bool stageIsZup =
                (UsdGeomGetStageUpAxis(stage)==UsdGeomTokens->z);

            interface.setAttr("info.usd.stageIsZup",
                              FnKat::IntAttribute(stageIsZup));
            
            interface.setAttr("info.usdOpArgs", opArgs);
            interface.setAttr("info.usd.outputSession", usdInArgs->GetSessionAttr());
        }
        
        if (FnAttribute::IntAttribute(
                opArgs.getChildByName("setOpArgsToInfo")).getValue(0, false))
        {
            opArgs = FnAttribute::GroupBuilder()
                .update(opArgs)
                .del("setOpArgsToInfo")
                .build();
            
            interface.setAttr("info.usdOpArgs", opArgs);
            interface.setAttr("info.usd.outputSession", usdInArgs->GetSessionAttr());
        }

        bool verbose = usdInArgs->IsVerbose();

        // The next section only makes sense to execute on non-pseudoroot prims
        if (prim.GetPath() != SdfPath::AbsoluteRootPath())
        {
            if (!prim.IsLoaded()) {
                SdfPath pathToLoad = prim.GetPath();
                readerLock.unlock();
                prim = _LoadPrim(stage, pathToLoad, verbose);
                if (!prim) {
                    ERROR("load prim %s failed", pathToLoad.GetText());
                    return;
                }
                readerLock.lock();
            }

            //
            // If the staticScene attrs dictate that we should insert an empty
            // group at this location, configure the interface to build out the
            // current prim as if it were its own child and return before
            // executing any registered ops.
            //
            // Note, the inserted group will have the same name as the current
            // prim.
            //
            // Note, we assume that the staticScene contents are aware of the
            // inserted group, i.e. that the current contents apply to the empty
            // group and there is a child entry for the current prim.
            //

            FnKat::GroupAttribute staticScene =
                    interface.getOpArg("staticScene");
            if (FnKat::IntAttribute(staticScene.getChildByName(
                    "a.insertEmptyGroup")).getValue(0, false) == 1)
            {
                // Execute any ops contained within the staticScene args
                _ExecStaticSceneOps(interface, staticScene);

                const std::string primName = prim.GetName();

                // Same caveat as the one mentioned in BuildIntermediate...
                //
                // XXX In order for the prim's material hierarchy to get built
                // out correctly via the UsdInCore_LooksGroupOp, we'll need
                // to override the original 'rootLocation' and 'isolatePath'
                // UsdIn args.
                //
                ArgsBuilder ab;
                ab.update(usdInArgs);
                ab.rootLocation =
                        interface.getOutputLocationPath() + "/" + primName;
                ab.isolatePath = prim.GetPath().GetString();
                interface.createChild(
                    primName, "",
                    FnKat::GroupBuilder()
                        .update(opArgs)
                        .set("staticScene", staticScene.getChildByName("c." + primName))
                        .build(),
                    FnKat::GeolibCookInterface::ResetRootFalse,
                    new UsdKatanaUsdInPrivateData(prim, ab.build(), privateData),
                    UsdKatanaUsdInPrivateData::Delete);
                return;
            }

            // When in "as sources and instances" mode, scan for instances
            // and prototypes at each location that contains a payload.
            if (prim.HasAuthoredPayloads() &&
                !usdInArgs->GetPrePopulate() &&
                FnAttribute::StringAttribute(
                    interface.getOpArg("instanceMode")
                    ).getValue("expanded", false) == 
                "as sources and instances")
            {
                FnKat::GroupAttribute prototypeMapping =
                    UsdKatanaUtils::BuildInstancePrototypeMapping(prim.GetStage(), prim.GetPath());
                FnKat::StringAttribute prototypeParentPath(prim.GetPath().GetString());
                if (prototypeMapping.isValid() && prototypeMapping.getNumberOfChildren())
                {
                    opArgs = FnKat::GroupBuilder()
                                 .update(opArgs)
                                 .set("prototypeMapping", prototypeMapping)
                                 .set("prototypeParentPath", prototypeParentPath)
                                 .build();
                }
                else
                {
                    opArgs = FnKat::GroupBuilder().update(opArgs).del("prototypeMapping").build();
                }
            }

            //
            // Compute and set the 'bound' attribute.
            //
            // Note, bound computation is handled here because bounding
            // box computation requires caching for optimal performance.
            // Instead of passing around a bounding box cache everywhere
            // it's needed, we use the usdInArgs data strucutre for caching.
            //

            if (UsdKatanaUtils::IsBoundable(prim))
            {
                interface.setAttr("bound",
                                  _MakeBoundsAttribute(prim, *privateData));
            }

            //
            // Find and execute the core op that handles the USD type.
            //

            {
                std::string opName;
                const TfToken typeName = prim.GetTypeName();
                if (!UsdKatanaUsdInPluginRegistry::FindUsdType(typeName, &opName))
                {
                    // If there is no type registered, we search through the
                    // applied schemas to see if one of those has an op
                    // registered against them. We only expect one of these
                    // schemas to be registered against an import Op.
                    const auto& appliedSchemas = prim.GetAppliedSchemas();
                    bool foundRegisteredSchema = false;
                    for (auto& appliedSchemaName : appliedSchemas)
                    {
                        if (UsdKatanaUsdInPluginRegistry::FindSchema(appliedSchemaName, &opName))
                        {
                            if (foundRegisteredSchema)
                            {
                                FnLogWarn("Multiple schemas applied on prim at location "
                                          << prim.GetPath()
                                          << " which are registered against different input ops.");
                            }
                            foundRegisteredSchema = true;
                        }
                    }
                }
                if (!opName.empty())
                {
                    if (privateData)
                    {
                        if ((typeName.GetString() != "SkelRoot") ||
                            privateData->GetEvaluateUsdSkelBindings())
                        {
                            // roughly equivalent to execOp except that we
                            // can locally override privateData
                            UsdKatanaUsdInPluginRegistry::ExecuteOpDirectExecFnc(
                                opName, *privateData, opArgs, interface);

                            opArgs = privateData->updateExtensionOpArgs(opArgs);
                        }
                    }
                }
            }

            //
            // Find and execute the site-specific op that handles the USD type.
            //

            {
                std::string opName;
                if (UsdKatanaUsdInPluginRegistry::FindUsdTypeForSite(prim.GetTypeName(), &opName))
                {
                    if (!opName.empty()) {
                        if (privateData)
                        {
                            // roughly equivalent to execOp except that we can
                            // locally override privateData
                            UsdKatanaUsdInPluginRegistry::ExecuteOpDirectExecFnc(
                                opName, *privateData, opArgs, interface);
                            opArgs = privateData->updateExtensionOpArgs(opArgs);
                        }
                    }
                }
            }

            //
            // Find and execute the core kind op that handles the model kind.
            //

            bool execKindOp = FnKat::IntAttribute(
                interface.getOutputAttr("__UsdIn.execKindOp")).getValue(1, false);

            if (execKindOp)
            {
                TfToken kind;
                if (UsdModelAPI(prim).GetKind(&kind)) {
                    std::string opName;
                    if (UsdKatanaUsdInPluginRegistry::FindKind(kind, &opName))
                    {
                        if (!opName.empty()) {
                            if (privateData)
                            {
                                // roughly equivalent to execOp except that we can
                                // locally override privateData
                                UsdKatanaUsdInPluginRegistry::ExecuteOpDirectExecFnc(
                                    opName, *privateData, opArgs, interface);

                                opArgs = privateData->updateExtensionOpArgs(opArgs);
                            }
                        }
                    }
                }
            }

            //
            // Find and execute the site-specific kind op that handles 
            // the model kind.
            //

            if (_hasSiteKinds) {
                TfToken kind;
                if (UsdModelAPI(prim).GetKind(&kind)) {
                    std::string opName;
                    if (UsdKatanaUsdInPluginRegistry::FindKindForSite(kind, &opName))
                    {
                        if (!opName.empty()) {
                            if (privateData)
                            {
                                UsdKatanaUsdInPluginRegistry::ExecuteOpDirectExecFnc(
                                    opName, *privateData, opArgs, interface);
                                opArgs = privateData->updateExtensionOpArgs(opArgs);
                            }
                        }
                    }
                }
            }

            //
            // Read blind data. This is last because blind data opinions 
            // should always win.
            //

            UsdKatanaAttrMap attrs;
            UsdKatanaReadBlindData(UsdKatanaBlindDataObject(prim), attrs);
            attrs.toInterface(interface);

            //
            // Execute any ops contained within the staticScene args.
            //

            // Re-get the latest staticScene in case anything changed
            staticScene = interface.getOpArg("staticScene");
            _ExecStaticSceneOps(interface, staticScene);

        }   // if (prim.GetPath() != SdfPath::AbsoluteRootPath())
        
        bool skipAllChildren = FnKat::IntAttribute(
                interface.getOutputAttr("__UsdIn.skipAllChildren")).getValue(
                0, false);

        if (prim.IsPrototype() and FnKat::IntAttribute(
                opArgs.getChildByName("childOfIntermediate")
                ).getValue(0, false) == 1)
        {
            interface.setAttr("type",
                    FnKat::StringAttribute("instance source"));
            interface.setAttr("tabs.scenegraph.stopExpand", 
                    FnKat::IntAttribute(1));

            // XXX prototypes are simple placeholders and will not get read as
            // models, so we'll need to explicitly process their Looks in a
            // manner similar to what the UsdInCore_ModelOp does.
            UsdPrim lookPrim =
                    prim.GetChild(TfToken(
                    UsdKatanaTokens->katanaLooksScopeName));
            if (lookPrim)
            {
                interface.setAttr(
                    UsdKatanaTokens->katanaLooksChildNameExclusionAttrName.GetString(),
                    FnKat::IntAttribute(1));
                interface.createChild(UsdKatanaTokens->katanaLooksScopeName.GetString(),
                                      "UsdInCore_LooksGroupOp",
                                      FnKat::GroupAttribute(),
                                      FnKat::GeolibCookInterface::ResetRootTrue,
                                      new UsdKatanaUsdInPrivateData(
                                          lookPrim, privateData->GetUsdInArgs(), privateData),
                                      UsdKatanaUsdInPrivateData::Delete);
            }
        }

        if (prim.IsInstance())
        {
            UsdPrim prototype = prim.GetPrototype();
            interface.setAttr("info.usd.prototypePrimPath",
                              FnAttribute::StringAttribute(prototype.GetPrimPath().GetString()));

            FnAttribute::StringAttribute prototypePathAttr = opArgs.getChildByName(
                "prototypeMapping." + FnKat::DelimiterEncode(prototype.GetPrimPath().GetString()));
            if (prototypePathAttr.isValid())
            {
                std::string prototypePath = prototypePathAttr.getValue("", false);

                std::string prototypeParentPath =
                    FnAttribute::StringAttribute(opArgs.getChildByName("prototypeParentPath"))
                        .getValue("", false);
                if (prototypeParentPath == "/")
                {
                    prototypeParentPath = std::string();
                }

                if (!prototypePath.empty())
                {
                    interface.setAttr(
                            "type", FnKat::StringAttribute("instance"));
                    // If we are a nested instance, the UsdInArgs' root location
                    // will be different from the "original" root location
                    // specified on the UsdIn node, since the
                    // BuildIntermediate op reinvokes UsdIn with modified
                    // UsdInArgs when building out prototypes.
                    // We can get at the original root location by querying the
                    // raw op args rather than the UsdInArgs.
                    std::string rootLocationPath = FnAttribute::StringAttribute(
                        opArgs.getChildByName("location")).getValue(
                            interface.getRootLocationPath(), false);
                    interface.setAttr(
                        "geometry.instanceSource",
                        FnAttribute::StringAttribute(rootLocationPath + prototypeParentPath +
                                                     "/Prototypes/" + prototypePath));

                    // XXX, ConstraintGroups are still made for models
                    //      that became instances. Need to suppress creation
                    //      of that stuff
                    interface.deleteChildren();
                    skipAllChildren = true;
                }
            }
        }

        if (!prim.IsPseudoRoot())
        {
            const TfTokenVector& lookTokens = UsdKatanaUtils::GetLookTokens();
            // when checking for a looks group, swap in the prototype if the prim is an instance
            UsdPrim resolvedPrim = (prim.IsInstance() && !privateData->GetPrototypePath().IsEmpty())
                                       ? prim.GetPrototype()
                                       : prim;
            for (const TfToken& lookToken : lookTokens)
            {
                UsdPrim lookPrim = resolvedPrim.GetChild(lookToken);
                if (lookPrim)
                {
                    FnKat::GroupAttribute childOpArgs = opArgs;

                    // Across instances, we won't easily be able to over this attr onto
                    // the Looks scope. We'll check for it on the parent also.
                    UsdAttribute keyAttr = prim.GetAttribute(TfToken("sharedLooksCacheKey"));
                    bool isValid = keyAttr.IsValid();
                    if (isValid)
                    {
                        std::string cacheKey;
                        keyAttr.Get(&cacheKey);

                        if (!cacheKey.empty())
                        {
                            childOpArgs =
                                FnKat::GroupBuilder()
                                    .update(childOpArgs)
                                    .set("sharedLooksCacheKey", FnKat::StringAttribute(cacheKey))
                                    .build();
                        }
                    }
                    interface.setAttr(
                        UsdKatanaTokens->katanaLooksChildNameExclusionAttrName.GetString() +
                            lookToken.GetString(),
                        FnKat::IntAttribute(1));
                    interface.createChild(lookToken.GetString(),
                                          "UsdInCore_LooksGroupOp",
                                          childOpArgs,
                                          FnKat::GeolibCookInterface::ResetRootTrue,
                                          new UsdKatanaUsdInPrivateData(
                                              lookPrim, privateData->GetUsdInArgs(), privateData),
                                          UsdKatanaUsdInPrivateData::Delete);
                }
            }
        }

        // advertise available variants for UIs to choose amongst
        UsdVariantSets variantSets = prim.GetVariantSets();
        std::vector<std::string> variantNames;
        std::vector<std::string> variantValues;
        variantSets.GetNames(&variantNames);
        TF_FOR_ALL(I, variantNames)
        {
            const std::string & variantName = (*I);
            UsdVariantSet variantSet = variantSets.GetVariantSet(variantName);
            variantValues = variantSet.GetVariantNames();
            
            interface.setAttr("info.usd.variants." + variantName,
                    FnAttribute::StringAttribute(variantValues, 1));
            
            interface.setAttr("info.usd.selectedVariants." + variantName,
                    FnAttribute::StringAttribute(
                            variantSet.GetVariantSelection()));
            
        }

        // Emit "Prototypes".
        // When prepopulating, these will be discovered and emitted under
        // the root.  Otherwise, they will be discovered incrementally
        // as each payload is loaded, and we emit them under the payload's
        // location.
        if (interface.atRoot() ||
            (prim.HasAuthoredPayloads() && !usdInArgs->GetPrePopulate())) {
            FnKat::GroupAttribute prototypeMapping = opArgs.getChildByName("prototypeMapping");
            if (prototypeMapping.isValid() && prototypeMapping.getNumberOfChildren())
            {
                FnGeolibServices::StaticSceneCreateOpArgsBuilder sscb(false);
                
                struct usdPrimInfo
                {
                    std::vector<std::string> usdPrimPathValues;
                    std::vector<std::string> usdPrimNameValues;
                };
                
                std::map<std::string, usdPrimInfo> primInfoPerLocation;

                for (size_t i = 0, e = prototypeMapping.getNumberOfChildren(); i != e; ++i)
                {
                    std::string prototypeName =
                        FnKat::DelimiterDecode(prototypeMapping.getChildName(i));

                    std::string katanaPath =
                        FnKat::StringAttribute(prototypeMapping.getChildByIndex(i))
                            .getValue("", false);

                    if (katanaPath.empty())
                    {
                        continue;
                    }

                    katanaPath = "Prototypes/" + katanaPath;

                    std::string leafName =
                            FnGeolibUtil::Path::GetLeafName(katanaPath);
                    std::string locationParent =
                            FnGeolibUtil::Path::GetLocationParent(katanaPath);
                    
                    auto & entry = primInfoPerLocation[locationParent];

                    entry.usdPrimPathValues.push_back(prototypeName);
                    entry.usdPrimNameValues.push_back(leafName);
                }
                
                for (const auto & I : primInfoPerLocation)
                {
                    const auto & locationParent = I.first;
                    const auto & entry = I.second;
                    
                    sscb.setAttrAtLocation(locationParent, "usdPrimPath",
                            FnKat::StringAttribute(entry.usdPrimPathValues));
                    sscb.setAttrAtLocation(locationParent, "usdPrimName",
                            FnKat::StringAttribute(entry.usdPrimNameValues));
                }
                
                FnKat::GroupAttribute childAttrs =
                    sscb.build().getChildByName("c");
                for (int64_t i = 0; i < childAttrs.getNumberOfChildren(); ++i)
                {
                    interface.createChild(childAttrs.getChildName(i), "UsdIn.BuildIntermediate",
                                          FnKat::GroupBuilder()
                                              .update(opArgs)
                                              .set("staticScene", childAttrs.getChildByIndex(i))
                                              .build(),
                                          FnKat::GeolibCookInterface::ResetRootFalse,
                                          new UsdKatanaUsdInPrivateData(usdInArgs->GetRootPrim(),
                                                                        usdInArgs, privateData),
                                          UsdKatanaUsdInPrivateData::Delete);
                }
            }
        }

        if (privateData)
        {
            opArgs = UsdKatanaUsdInPluginRegistry::ExecuteLocationDecoratorOps(*privateData, opArgs,
                                                                               interface);
        }

        if (!skipAllChildren)
        {
            std::set<std::string> childrenToSkip;
            FnKat::GroupAttribute childOps = interface.getOutputAttr(
                "__UsdIn.skipChild");
            if (childOps.isValid()) {
                for (int64_t i = 0; i < childOps.getNumberOfChildren(); i++) {
                    std::string childName = childOps.getChildName(i);
                    bool shouldSkip = FnKat::IntAttribute(
                        childOps.getChildByIndex(i)).getValue(0, false);
                    if (shouldSkip) {
                        childrenToSkip.insert(childName);
                    }
                }
            }

            // If the prim is an instance (has a valid prototype path)
            // we replace the current prim with the prototype prim before
            // iterating on the children.
            //
            if (prim.IsInstance() && !privateData->GetPrototypePath().IsEmpty())
            {
                const UsdPrim& prototypePrim = prim.GetPrototype();
                if (!prototypePrim)
                {
                    ERROR(
                        "USD Prim is advertised as an instance "
                        "but prototype prim cannot be found.");
                }
                else
                {
                    prim = prototypePrim;
                }
            }

            // create children
            auto predicate = UsdPrimIsActive && !UsdPrimIsAbstract;
            if (interface.getNumInputs() == 0) {
                // Require a defining specifier on prims if there is no input.
                predicate = UsdPrimIsDefined && predicate;
            }
            TF_FOR_ALL(childIter, prim.GetFilteredChildren(predicate))
            {
                const UsdPrim& child = *childIter;
                const std::string& childName = child.GetName();

                if (childrenToSkip.count(childName)) {
                    continue;
                }

                // If we allow prims without a defining specifier then
                // also check that the prim exists in the input so we
                // have something to override.
                if (!child.HasDefiningSpecifier()) {
                    if (!interface.doesLocationExist(childName)) {
                        // Skip over with no def.
                        continue;
                    }
                }
                // If the child is a light filter, skip adding it here. It will be added through a
                // relationship from a light prim should it be needed.
                bool skipForFilter = false;
                std::unordered_set<std::string> shaderIds =
                    UsdKatanaUtils::GetShaderIds(child, privateData->GetCurrentTime());
                for (const std::string& shaderId : shaderIds)
                {
                    auto node = UsdKatanaUtils::GetShaderNodeFromShaderId(shaderId);
                    if (node && node->GetContext() == TfToken("lightFilter"))
                    {
                        skipForFilter = true;
                        break;
                    }
                }
                if (!skipForFilter)
                {
                    interface.createChild(
                        childName,
                        "",
                        FnKat::GroupBuilder()
                            .update(opArgs)
                            .set("staticScene", opArgs.getChildByName("staticScene.c." + childName))
                            .build(),
                        FnKat::GeolibCookInterface::ResetRootFalse,
                        new UsdKatanaUsdInPrivateData(child, usdInArgs, privateData),
                        UsdKatanaUsdInPrivateData::Delete);
                }
            }
        }

        // keep things around if we are verbose
        if (!verbose) {
            interface.deleteAttr("__UsdIn");
        }
    }

private:
    /*
     * Get the write lock and load the USD prim.
     */
    static UsdPrim _LoadPrim(
            const UsdStageRefPtr& stage, 
            const SdfPath& pathToLoad,
            bool verbose)
    {
        boost::unique_lock<boost::upgrade_mutex>
            writerLock(UsdKatanaGetStageLock());

        if (verbose) {
            FnLogInfo(TfStringPrintf(
                        "%s was not loaded. .. Loading.", 
                        pathToLoad.GetText()).c_str());
        }

        return stage->Load(pathToLoad);
    }

    static FnKat::DoubleAttribute _MakeBoundsAttribute(const UsdPrim& prim,
                                                       const UsdKatanaUsdInPrivateData& data)
    {
        if (prim.GetPath() == SdfPath::AbsoluteRootPath()) {
            // Special-case to pre-empt coding errors.
            return FnKat::DoubleAttribute();
        }
        const std::vector<double>& motionSampleTimes =
            data.GetMotionSampleTimes();
        UsdPrim computePrim = prim;
        if (prim.IsInPrototype())
        {
            const SdfPath instancePath = computePrim.GetPath().ReplacePrefix(
                data.GetPrototypePath(), data.GetInstancePath());
            computePrim = prim.GetStage()->GetPrimAtPath(instancePath);
        }
        std::vector<GfBBox3d> bounds =
            data.GetUsdInArgs()->ComputeBounds(computePrim, motionSampleTimes);

        bool hasInfiniteBounds = false;
        bool isMotionBackward = motionSampleTimes.size() > 1 &&
            motionSampleTimes.front() > motionSampleTimes.back();
        FnKat::DoubleAttribute boundsAttr = UsdKatanaUtils::ConvertBoundsToAttribute(
            bounds, motionSampleTimes, isMotionBackward, &hasInfiniteBounds);

        // Report infinite bounds as a warning.
        if (hasInfiniteBounds) {
            FnLogWarn("Infinite bounds found at "<<prim.GetPath().GetString());
        }

        return boundsAttr;
    }

    static void
    _ExecStaticSceneOps(
            FnKat::GeolibCookInterface& interface,
            FnKat::GroupAttribute& staticScene)
    {
        FnKat::GroupAttribute opsGroup = staticScene.getChildByName("x");
        if (opsGroup.isValid())
        {
            for (int childindex = 0; childindex < opsGroup.getNumberOfChildren();
                    ++childindex)
            {
                FnKat::GroupAttribute entry =
                        opsGroup.getChildByIndex(childindex);

                if (!entry.isValid())
                {
                    continue;
                }

                FnKat::StringAttribute subOpType =
                        entry.getChildByName("opType");

                FnKat::GroupAttribute subOpArgs =
                        entry.getChildByName("opArgs");

                if (!subOpType.isValid() || !subOpArgs.isValid())
                {
                    continue;
                }

                interface.execOp(subOpType.getValue("", false), subOpArgs);
            }
        }
    }

    static bool _hasSiteKinds;
};

bool UsdInOp::_hasSiteKinds = false;

//-----------------------------------------------------------------------------

/*
 * DEPRECATED
 * This op bootstraps the primary UsdIn op in order to have
 * GeolibPrivateData available at the root op location in UsdIn. Since the
 * GeolibCookInterface API does not currently have the ability to pass
 * GeolibPrivateData via execOp, and we must exec all of the registered plugins
 * to process USD prims, we instead pre-build the GeolibPrivateData for the
 * root location to ensure it is available.
 */
class UsdInBootstrapOp : public FnKat::GeolibOp
{

public:

    static void setup(FnKat::GeolibSetupInterface &interface)
    {
        interface.setThreading(
                FnKat::GeolibSetupInterface::ThreadModeConcurrent);
    }

    static void cook(FnKat::GeolibCookInterface &interface)
    {
        ERROR("UsdInBootstrapOp is deprecated please use ExecuteOpDirectExecFnc instead.");
        return;

        interface.stopChildTraversal();

        boost::shared_lock<boost::upgrade_mutex> 
            readerLock(UsdKatanaGetStageLock());
            
        FnKat::GroupAttribute additionalOpArgs;
        UsdKatanaUsdInArgsRefPtr usdInArgs =
            InitUsdInArgs(interface.getOpArg(), additionalOpArgs, interface.getRootLocationPath());

        if (!usdInArgs) {
            ERROR("Could not initialize UsdIn usdInArgs.");
            return;
        }
        
        if (!usdInArgs->GetErrorMessage().empty())
        {
            ERROR("%s", usdInArgs->GetErrorMessage().c_str());
            return;
        }

        FnKat::GroupAttribute opArgs = FnKat::GroupBuilder()
            .update(interface.getOpArg())
            .deepUpdate(additionalOpArgs)
            .set("setOpArgsToInfo", FnAttribute::IntAttribute(1))
            .build();

        // Extract the basename (string after last '/') from the location
        // the UsdIn op is configured to run at such that we can create
        // that child and exec the UsdIn op on it.
        //
        std::vector<std::string> tokens;
        pystring::split(usdInArgs->GetRootLocationPath(), tokens, "/");

        if (tokens.empty())
        {
            ERROR(
                "Could not initialize UsdIn op with "
                "UsdIn.Bootstrap op.");
            return;
        }

        const std::string& rootName = tokens.back();

        interface.createChild(rootName, "UsdIn", opArgs, FnKat::GeolibCookInterface::ResetRootTrue,
                              new UsdKatanaUsdInPrivateData(usdInArgs->GetRootPrim(), usdInArgs,
                                                            NULL /* parentData */),
                              UsdKatanaUsdInPrivateData::Delete);
    }

};

/*
 * DEPRECATED
 * This op bootstraps the primary UsdIn op in order to have
 * GeolibPrivateData available at the root op location in UsdIn. Since the
 * GeolibCookInterface API does not currently have the ability to pass
 * GeolibPrivateData via execOp, and we must exec all of the registered plugins
 * to process USD prims, we instead pre-build the GeolibPrivateData for the
 * root location to ensure it is available.
 */
class UsdInMaterialGroupBootstrapOp : public FnKat::GeolibOp
{

public:

    static void setup(FnKat::GeolibSetupInterface &interface)
    {
        interface.setThreading(
                FnKat::GeolibSetupInterface::ThreadModeConcurrent);
    }

    static void cook(FnKat::GeolibCookInterface &interface)
    {
        ERROR(
            "UsdInMaterialGroupBootstrapOp is deprecated please use ExecuteOpDirectExecFnc "
            "instead.");
        return;

        interface.stopChildTraversal();

        boost::shared_lock<boost::upgrade_mutex> 
            readerLock(UsdKatanaGetStageLock());
            
        FnKat::GroupAttribute additionalOpArgs;
        UsdKatanaUsdInArgsRefPtr usdInArgs =
            InitUsdInArgs(interface.getOpArg(), additionalOpArgs, interface.getRootLocationPath());

        if (!usdInArgs) {
            ERROR("Could not initialize UsdIn usdInArgs.");
            return;
        }
        
        if (!usdInArgs->GetErrorMessage().empty())
        {
            ERROR("%s", usdInArgs->GetErrorMessage().c_str());
            return;
        }

        FnKat::GroupAttribute opArgs = FnKat::GroupBuilder()
            .update(interface.getOpArg())
            .deepUpdate(additionalOpArgs)
            .build();

        UsdKatanaUsdInPrivateData privateData(usdInArgs->GetRootPrim(), usdInArgs,
                                              NULL /* parentData */);

        UsdKatanaUsdInPluginRegistry::ExecuteOpDirectExecFnc("UsdInCore_LooksGroupOp", privateData,
                                                             opArgs, interface);
    }

};

class UsdInBuildIntermediateOp : public FnKat::GeolibOp
{
public:
    static void setup(FnKat::GeolibSetupInterface &interface)
    {
        interface.setThreading(
                FnKat::GeolibSetupInterface::ThreadModeConcurrent);
    }

    static void cook(FnKat::GeolibCookInterface &interface)
    {
        UsdKatanaUsdInPrivateData* privateData =
            static_cast<UsdKatanaUsdInPrivateData*>(interface.getPrivateData());

// If we are exec'ed from katana 2.x from an op which doesn't have
// UsdKatanaUsdInPrivateData, we need to build some. We normally avoid
// this case by using the execDirect -- but some ops need to call
// UsdInBuildIntermediateOp via execOp. In 3.x, they can (and are
// required to) provide the private data.
#if KATANA_VERSION_MAJOR < 3
        
        // We may be constructing the private data locally -- in which case
        // it will not be destroyed by the Geolib runtime.
        // This won't be used directly but rather just filled if the private
        // needs to be locally built.
        std::unique_ptr<UsdKatanaUsdInPrivateData> localPrivateData;

        if (!privateData)
        {
            
            FnKat::GroupAttribute additionalOpArgs;
            auto usdInArgs = UsdInOp::InitUsdInArgs(interface.getOpArg(), additionalOpArgs,
                                                    interface.getRootLocationPath());
            auto opArgs = FnKat::GroupBuilder()
                .update(interface.getOpArg())
                .deepUpdate(additionalOpArgs)
                .build();
            
            // Construct local private data if none was provided by the parent.
            // This is a legitmate case for the root of the scene -- most
            // relevant with the isolatePath pointing at a deeper scope which
            // may have meaningful type/kind ops.
            
            if (usdInArgs->GetStage())
            {
                localPrivateData.reset(new UsdKatanaUsdInPrivateData(usdInArgs->GetRootPrim(),
                                                                     usdInArgs, privateData));
                privateData = localPrivateData.get();
            }
            else
            {
                //TODO, warning
                return;
            }
            
        }

#endif

        UsdKatanaUsdInArgsRefPtr usdInArgs = privateData->GetUsdInArgs();

        FnKat::GroupAttribute staticScene =
                interface.getOpArg("staticScene");

        FnKat::GroupAttribute attrsGroup = staticScene.getChildByName("a");

        FnKat::StringAttribute primPathAttr =
                attrsGroup.getChildByName("usdPrimPath");
        FnKat::StringAttribute primNameAttr =
                attrsGroup.getChildByName("usdPrimName");

        
        std::set<std::string> createdChildren;
        
        // If prim attrs are present, use them to build out the usd prim.
        // Otherwise, build out a katana group.
        //
        if (primPathAttr.isValid())
        {
            attrsGroup = FnKat::GroupBuilder()
                .update(attrsGroup)
                .del("usdPrimPath")
                .del("usdPrimName")
                .build();

            
            auto usdPrimPathValues = primPathAttr.getNearestSample(0);
            
            
            
            for (size_t i = 0; i < usdPrimPathValues.size(); ++i)
            {
                std::string primPath(usdPrimPathValues[i]);
                if (!SdfPath::IsValidPathString(primPath))
                {
                    continue;
                }
                
                // Get the usd prim at the given source path.
                //
                UsdPrim prim = usdInArgs->GetStage()->GetPrimAtPath(
                        SdfPath(primPath));

                // Get the desired name for the usd prim; if one isn't provided,
                // ask the prim directly.
                //
                std::string nameToUse = prim.GetName();
                if (primNameAttr.getNumberOfValues() > static_cast<int64_t>(i))
                {
                    auto primNameAttrValues = primNameAttr.getNearestSample(0);
                    
                    std::string primName = primNameAttrValues[i];
                    if (!primName.empty())
                    {
                        nameToUse = primName;
                    }
                }

                // XXX In order for the prim's material hierarchy to get built
                // out correctly via the UsdInCore_LooksGroupOp, we'll need
                // to override the original 'rootLocation' and 'isolatePath'
                // UsdIn args.
                //
                ArgsBuilder ab;
                ab.update(usdInArgs);
                ab.rootLocation =
                        interface.getOutputLocationPath() + "/" + nameToUse;
                ab.isolatePath = primPath;

                // If the child we are making has intermediate children,
                // send those along. This currently happens with point
                // instancer prototypes and the children of Looks groups.
                //
                FnKat::GroupAttribute childrenGroup =
                        staticScene.getChildByName("c." + nameToUse);

                createdChildren.insert(nameToUse);
                // Build the prim using UsdIn.
                //
                interface.createChild(nameToUse, "UsdIn",
                                      FnKat::GroupBuilder()
                                          .update(interface.getOpArg())
                                          .set("childOfIntermediate", FnKat::IntAttribute(1))
                                          .set("staticScene", childrenGroup)
                                          .build(),
                                      FnKat::GeolibCookInterface::ResetRootFalse,
                                      new UsdKatanaUsdInPrivateData(prim, ab.build(), privateData),
                                      UsdKatanaUsdInPrivateData::Delete);
            }
        }
        
        FnKat::GroupAttribute childrenGroup =
            staticScene.getChildByName("c");
        for (size_t i = 0, e = childrenGroup.getNumberOfChildren(); i != e;
                 ++i)
        {
            FnKat::GroupAttribute childGroup =
                    childrenGroup.getChildByIndex(i);

            if (!childGroup.isValid())
            {
                continue;
            }

            std::string childName = childrenGroup.getChildName(i);
            
            if (createdChildren.find(childName) != createdChildren.end())
            {
                continue;
            }
            
            // Build the intermediate group using the same op.
            //
            interface.createChild(
                childrenGroup.getChildName(i), "",
                FnKat::GroupBuilder()
                    .update(interface.getOpArg())
                    .set("staticScene", childGroup)
                    .build(),
                FnKat::GeolibCookInterface::ResetRootFalse,
                new UsdKatanaUsdInPrivateData(usdInArgs->GetRootPrim(), usdInArgs, privateData),
                UsdKatanaUsdInPrivateData::Delete);
        }
        

        // Apply local attrs.
        //
        for (size_t i = 0, e = attrsGroup.getNumberOfChildren(); i != e; ++i)
        {
            interface.setAttr(attrsGroup.getChildName(i),
                    attrsGroup.getChildByIndex(i));
        }
        
    }
};

class UsdInAddViewerProxyOp : public FnKat::GeolibOp
{
public:
    static void setup(FnKat::GeolibSetupInterface &interface)
    {
        interface.setThreading(
                FnKat::GeolibSetupInterface::ThreadModeConcurrent);
    }

    static void cook(FnKat::GeolibCookInterface &interface)
    {
        interface.setAttr(
            "proxies",
            UsdKatanaUtils::GetViewerProxyAttr(
                FnKat::DoubleAttribute(interface.getOpArg("currentTime")).getValue(0.0, false),
                FnKat::StringAttribute(interface.getOpArg("fileName")).getValue("", false),

                FnKat::StringAttribute(interface.getOpArg("isolatePath")).getValue("", false),

                FnKat::StringAttribute(interface.getOpArg("rootLocation")).getValue("", false),
                interface.getOpArg("session"),
                FnKat::StringAttribute(interface.getOpArg("ignoreLayerRegex"))
                    .getValue("", false)));
    }
};

//------------------------------------------------------------------------------

/*
 * This op updates the global lists "globals.cameraList" and "lightList" at the
 * /root/world Katana scenegraph location.
 */
class UsdInUpdateGlobalListsOp : public FnKat::GeolibOp
{
public:
    static void setup(FnKat::GeolibSetupInterface& interface)
    {
        interface.setThreading(FnKat::GeolibSetupInterface::ThreadModeConcurrent);
    }

    static void cook(FnKat::GeolibCookInterface& interface)
    {
        interface.stopChildTraversal();

        UsdKatanaUsdInPrivateData* privateData =
            static_cast<UsdKatanaUsdInPrivateData*>(interface.getPrivateData());
        UsdKatanaUsdInArgsRefPtr usdInArgs;
        if (privateData) {
            usdInArgs = privateData->GetUsdInArgs();
        } else {
            FnKat::GroupAttribute additionalOpArgs;
            usdInArgs = InitUsdInArgs(interface.getOpArg(), additionalOpArgs,
                                      interface.getRootLocationPath());
        }
        if(!usdInArgs)
        {
            return;
        }

        UsdStagePtr stage = usdInArgs->GetStage();
        if(!stage)
        {
            return;
        }

        // Extract camera paths.
        SdfPathVector cameraPaths = UsdKatanaUtils::FindCameraPaths(stage);
        FnKat::StringBuilder cameraListBuilder;
        for (const SdfPath& cameraPath : cameraPaths)
        {
            const std::string& path = cameraPath.GetString();
            if (path.find(usdInArgs->GetIsolatePath()) != std::string::npos)
            {
                cameraListBuilder.push_back(
                    TfNormPath(usdInArgs->GetRootLocationPath() + "/" +
                               path.substr(usdInArgs->GetIsolatePath().size())));
            }
        }

        FnKat::StringAttribute cameraListAttr = cameraListBuilder.build();
        if (cameraListAttr.getNumberOfValues() > 0)
        {
            interface.extendAttr("globals.cameraList", cameraListAttr);
        }

        // Extract light paths.
        // If isolatePath is used, we check that any of the light paths found
        // are children of it before adding them to the lists. This is
        // because we generate the light list using the cache so those paths
        // may not actually exist.
        const std::string& isolatePathString = usdInArgs->GetIsolatePath();
        const SdfPath isolatePath =
            isolatePathString.empty() ? SdfPath::AbsoluteRootPath() : SdfPath(isolatePathString);
        SdfPathVector lightPaths = UsdKatanaUtils::FindLightPaths(stage);
        stage->LoadAndUnload(SdfPathSet(lightPaths.begin(), lightPaths.end()), SdfPathSet());
        UsdKatanaUtilsLightListEditor lightListEditor(interface, usdInArgs);
        for (const SdfPath& lightPath : lightPaths)
        {
            if (lightPath.HasPrefix(isolatePath))
            {
                lightListEditor.SetPath(lightPath);
                UsdKatanaUsdInPluginRegistry::ExecuteLightListFncs(lightListEditor);
            }
        }

        lightListEditor.Build();
    }
};

//------------------------------------------------------------------------------

class FlushStageFnc : public Foundry::Katana::AttributeFunction
{
public:
    static FnAttribute::Attribute run(FnAttribute::Attribute args)
    {
        boost::upgrade_lock<boost::upgrade_mutex>
                readerLock(UsdKatanaGetStageLock());
        
        FnKat::GroupAttribute additionalOpArgs;
        auto usdInArgs = InitUsdInArgs(args, additionalOpArgs, "/root");
        if (usdInArgs)
        {
            boost::upgrade_to_unique_lock<boost::upgrade_mutex>
                    writerLock(readerLock);
            UsdKatanaCache::GetInstance().FlushStage(usdInArgs->GetStage());
        }
        
        
        return FnAttribute::Attribute();
    }

};

//-----------------------------------------------------------------------------

DEFINE_GEOLIBOP_PLUGIN(UsdInOp)
DEFINE_GEOLIBOP_PLUGIN(UsdInBootstrapOp)
DEFINE_GEOLIBOP_PLUGIN(UsdInMaterialGroupBootstrapOp)
DEFINE_GEOLIBOP_PLUGIN(UsdInBuildIntermediateOp)
DEFINE_GEOLIBOP_PLUGIN(UsdInAddViewerProxyOp)
DEFINE_GEOLIBOP_PLUGIN(UsdInUpdateGlobalListsOp);
DEFINE_ATTRIBUTEFUNCTION_PLUGIN(FlushStageFnc);

void registerPlugins()
{
    REGISTER_PLUGIN(UsdInOp, "UsdIn", 0, 1);
    REGISTER_PLUGIN(UsdInBootstrapOp, "UsdIn.Bootstrap", 0, 1);
    REGISTER_PLUGIN(UsdInMaterialGroupBootstrapOp, "UsdIn.BootstrapMaterialGroup", 0, 1);
    REGISTER_PLUGIN(UsdInBuildIntermediateOp, "UsdIn.BuildIntermediate", 0, 1);
    REGISTER_PLUGIN(UsdInAddViewerProxyOp, "UsdIn.AddViewerProxy", 0, 1);
    REGISTER_PLUGIN(UsdInUpdateGlobalListsOp, "UsdIn.UpdateGlobalLists", 0, 1);
    REGISTER_PLUGIN(FlushStageFnc, "UsdIn.FlushStage", 0, 1);

    UsdKatanaBootstrap();
    VtKatanaBootstrap();
}

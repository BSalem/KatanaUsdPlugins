# These files began life as part of the main USD distribution
# https://github.com/PixarAnimationStudios/USD.
# In 2019, Foundry and Pixar agreed Foundry should maintain and curate
# these plug-ins, and they moved to
# https://github.com/TheFoundryVisionmongers/KatanaUsdPlugins
# under the same Modified Apache 2.0 license, as shown below.
#
# Copyright 2016 Pixar
#
# Licensed under the Apache License, Version 2.0 (the "Apache License")
# with the following modification; you may not use this file except in
# compliance with the Apache License and the following modification to it:
# Section 6. Trademarks. is deleted and replaced with:
#
# 6. Trademarks. This License does not grant permission to use the trade
#    names, trademarks, service marks, or product names of the Licensor
#    and its affiliates, except as required to comply with Section 4(c) of
#    the License and to reproduce the content of the NOTICE file.
#
# You may obtain a copy of the Apache License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the Apache License with the above modification is
# distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied. See the Apache License for the specific
# language governing permissions and limitations under the Apache License.
#
from Katana import (
    Nodes3DAPI,
    NodegraphAPI,
    RenderingAPI,
    FnAttribute,
    FnGeolibServices,
)

# pylint: disable=function-redefined

def getScenegraphLocation(self, frameTime):
    return self.getParameter('location').getValue(frameTime)

# node type builder for a our new node type
nb = Nodes3DAPI.NodeTypeBuilder('UsdIn')

# group builder for the node parameters
gb = FnAttribute.GroupBuilder()

gb.set('fileName', '')
nb.setHintsForParameter('fileName', {
    'help' : 'The USD file to read.',
    'widget':'assetIdInput',
    'fileTypes':'usd|usda|usdc|usdz',
})

gb.set('assetResolverContext', '')
nb.setHintsForParameter('assetResolverContext', {
    'help': """
        Sets the Asset Resolver Context which will be bound when opening the stage. If this is not
        specified, a default context is used based on the Asset Resolver for the fileName parameter.
    """,
})

gb.set('location', '/root/world/geo')
nb.setHintsForParameter('location', {
    'widget' : 'scenegraphLocation',
    'help' : 'The Katana scenegraph location to load the USD contents.',
})

gb.set('isolatePath', '')
nb.setHintsForParameter('isolatePath', {
    'help' : 'Load only the USD contents below the specified USD prim path.',
})


_offForArchiveCondVis = {
    'conditionalVisOp' : 'equalTo',
    'conditionalVisPath' : '../asArchive',
    'conditionalVisValue' : '0',
}


gb.set('variants', '')
nb.setHintsForParameter('variants', {
    # 'conditionalVisOp': 'notEqualTo',
    # 'conditionalVisPath': '../variants', # can't really point to self... :(
    # 'conditionalVisValue': '',
    'helpAlert': 'warning',
    'help' : 'DEPRECATED! Use UsdInVariantSelect instead.'\
        ' Specify variant '\
        'selections. Variant selections are specified via whitespace-separated'\
        ' variant selection paths. Example: /Foo{X=Y} /Bar{Z=w}',

    'conditionalVisOps' : _offForArchiveCondVis,

})

gb.set('ignoreLayerRegex', '')
nb.setHintsForParameter('ignoreLayerRegex', {
    'help' : 'Ignore matching USD layers during USD scene composition.',
    'conditionalVisOps' : _offForArchiveCondVis,
})

gb.set('motionSampleTimes', '')
nb.setHintsForParameter('motionSampleTimes', {
    'help' : 'Specify motion sample times to load. The default behavior is no motion samples (only load current time 0).',
    'conditionalVisOps' : _offForArchiveCondVis,
})

gb.set('instanceMode', 'expanded')
nb.setHintsForParameter('instanceMode', {
    'widget' : 'popup',
    'options' : ['expanded', 'as sources and instances'],
    'help' : """
      When using <i>expanded</i> USD instances will be unrolled as though
      the children of their prototypes were naturally present.
      </p>
      When using <i>as sources and instances</i>, prototypes will be created
      under a sibling of World named /Prototypes. Instanced USD prims will
      be of type "instance" and have no children.
    """,
    'conditionalVisOps' : _offForArchiveCondVis,
})




gb.set('usePurposeBasedMaterialBinding', 0)
nb.setHintsForParameter('usePurposeBasedMaterialBinding', {
    'widget' : 'boolean',
})

gb.set('additionalBindingPurposeNames', FnAttribute.StringAttribute([], 1))

nb.setHintsForParameter('additionalBindingPurposeNames', {
    'widget' : 'sortableArray',
    'conditionalVisOp' : 'notEqualTo',
    'conditionalVisPath' : '../usePurposeBasedMaterialBinding',
    'conditionalVisValue' : '',
})



gb.set('prePopulate', FnAttribute.IntAttribute(1))
nb.setHintsForParameter('prePopulate', {
    'widget' : 'checkBox',
    'help' : """
      Controls USD pre-population.  Pre-population loads all payloads
      and pre-populates the stage.  Assuming the entire stage will be
      needed, this is more efficient since USD can use its internal
      multithreading.
    """,
    'conditionalVisOps' : _offForArchiveCondVis,
    'constant' : True,

})

gb.set('verbose', 0)
nb.setHintsForParameter('verbose', {
    'widget' : 'checkBox',
    'help' : 'Output info during USD scene composition and scenegraph generation.',
    'conditionalVisOps' : _offForArchiveCondVis,
    'constant' : True,
})



gb.set('asArchive', 0)
nb.setHintsForParameter('asArchive', {
    'widget' : 'checkBox',
    'help' : """
        If enabled, the specified location will be of type "usd archive" rather
        than loaded directly into the katana scene -- optionally with a proxy.
    """,
    'constant' : True,
})

gb.set('includeProxyForArchive', 1)
nb.setHintsForParameter('includeProxyForArchive', {
    'widget' : 'checkBox',
    'help' : """
        If enabled, the specified location will be of type "usd archive" rather
        than loaded directly into the katana scene -- optionally with a proxy.
    """,

    'conditionalVisOp' : 'notEqualTo',
    'conditionalVisPath' : '../asArchive',
    'conditionalVisValue' : '0',

})

gb.set('evaluateUsdSkelBindings', 1)
nb.setHintsForParameter('evaluateUsdSkelBindings', {
    'widget' : 'checkBox',
    'help' : """
        If enabled the skinning will be applied to each prim that is bound to a
        skeleton.
    """,
    'constant' : True,
})

nb.setParametersTemplateAttr(gb.build())

#-----------------------------------------------------------------------------

# Given a graphState and the current parameter values, return the opArgs to
# UsdIn. This logic was previously exclusive to buildOpChain but was
# refactored to be callable directly -- initially in service of flushStage
def buildUsdInOpArgsAtGraphState(self, graphState):
    gb = FnAttribute.GroupBuilder()

    frameTime = graphState.getTime()

    gb.set('fileName',
            self.getParameter('fileName').getValue(frameTime))
    gb.set('assetResolverContext',
            self.getParameter('assetResolverContext').getValue(frameTime))
    gb.set('location',
            self.getParameter('location').getValue(frameTime))

    gb.set('isolatePath',
            self.getParameter('isolatePath').getValue(frameTime))

    gb.set('variants',
            self.getParameter('variants').getValue(frameTime))

    gb.set('ignoreLayerRegex',
            self.getParameter('ignoreLayerRegex').getValue(frameTime))

    gb.set('motionSampleTimes',
            self.getParameter('motionSampleTimes').getValue(frameTime))

    gb.set('verbose',
            int(self.getParameter('verbose').getValue(frameTime)))

    gb.set('instanceMode',
            self.getParameter('instanceMode').getValue(frameTime))

    gb.set('prePopulate',
            int(self.getParameter('prePopulate').getValue(frameTime)))

    loadedRenderers = RenderingAPI.RenderPlugins.GetRendererPluginNames()
    gb.set('outputTargets',
        FnAttribute.StringAttribute(loadedRenderers)
    )

    if self.getParameter('usePurposeBasedMaterialBinding').getValue(frameTime):
        purposes = ["",]

        for p in self.getParameter('additionalBindingPurposeNames').getChildren():
            v = p.getValue(frameTime).strip()
            if v:
                purposes.append(v)

        gb.set('materialBindingPurposes', FnAttribute.StringAttribute(purposes, 1))


    sessionValues = (
            graphState.getDynamicEntry("var:pxrUsdInSession"))
    if isinstance(sessionValues, FnAttribute.GroupAttribute):
        gb.set('session', sessionValues)


    gb.set('system', graphState.getOpSystemArgs())
    gb.set('processStageWideQueries', FnAttribute.IntAttribute(1))

    gb.set('setOpArgsToInfo', FnAttribute.IntAttribute(1))


    # check for any extra attributes or namespaces set downstream
    # via graph state
    extra = graphState.getDynamicEntry('var:usdExtraAttributesOrNamespaces')
    if extra:
        gb.set('extraAttributesOrNamespaces', extra)

    gb.set('evaluateUsdSkelBindings', int(self.getParameter(
        'evaluateUsdSkelBindings').getValue(frameTime)))

    argsOverride = graphState.getDynamicEntry('var:pxrUsdInArgs')
    if isinstance(argsOverride, FnAttribute.GroupAttribute):
        gb.update(argsOverride)

    usdInArgs = gb.build()

    return usdInArgs

nb.setCustomMethod('buildUsdInOpArgsAtGraphState', buildUsdInOpArgsAtGraphState)

#-----------------------------------------------------------------------------

kArgsCookTmpKeyToken = 'UsdIn_argsCookTmpKey'

# While it's possible to call buildUsdInOpArgsAtGraphState directly, it's
# usually more meaningful to call it with a graphState relative to a
# downstream node as UsdInVariantSelect (and its sibling) contribute to
# the graphState and resulting opArgs. This wraps up the inconvenience of
# tracking the graphState by injecting an extra entry into starting graphState
# which triggers buildOpChain to record its opArgs.
#
# NOTE: should a better way of tracking graphState appear in a future version
#       of Katana, the implementation details here are hidden within this
#       method.
def buildUsdInOpArgsFromDownstreamNode(
        self, downstreamNode, graphState, portIndex=0):
    if not hasattr(self, '_argsCookTmp'):
        self._argsCookTmp = {}

    key = (FnAttribute.GroupBuilder()
            .set('graphState', graphState.getOpSystemArgs())
            .set('node', hash(downstreamNode))
            .set('portIndex', portIndex)
            .build().getHash())

    # Set a dynamic entry that's not prefixed with "var:" so it won't show up
    # in op system args (or other graph state comparisons other than hash
    # uniqueness)
    useGraphState = graphState.edit().setDynamicEntry(kArgsCookTmpKeyToken,
            FnAttribute.StringAttribute(key)).build()

    # trigger a cook with this unique graph state
    Nodes3DAPI.CreateClient(downstreamNode,
            graphState=useGraphState, portIndex=portIndex)

    if key in self._argsCookTmp:
        result = self._argsCookTmp[key]
        return result
    else:
        # TODO, exception?
        pass

nb.setCustomMethod('buildUsdInOpArgsFromDownstreamNode', buildUsdInOpArgsFromDownstreamNode)

#-----------------------------------------------------------------------------

def flushStage(self, viewNode, graphState, portIndex=0):
    opArgs = self.buildUsdInOpArgsFromDownstreamNode(viewNode, graphState,
            portIndex=portIndex)

    if isinstance(opArgs, FnAttribute.GroupAttribute):
        FnGeolibServices.AttributeFunctionUtil.Run("UsdIn.FlushStage",
                opArgs)

nb.setCustomMethod('flushStage', flushStage)

#-----------------------------------------------------------------------------

def buildOpChain(self, interface):
    interface.setMinRequiredInputs(0)

    frameTime = interface.getGraphState().getTime()


    # simpler case for the archive
    if self.getParameter('asArchive').getValue(frameTime):
        sscb = FnGeolibServices.OpArgsBuilders.StaticSceneCreate(True)
        location = self.getScenegraphLocation(frameTime)
        sscb.createEmptyLocation(location, 'usd archive')


        gb = FnAttribute.GroupBuilder()


        for name in ('fileName', 'isolatePath', 'assetResolverContext'):
            gb.set(name, interface.buildAttrFromParam(
                    self.getParameter(name)))

        gb.set('currentTime', FnAttribute.DoubleAttribute(frameTime))

        attrs = gb.build()

        sscb.setAttrAtLocation(location, 'geometry', attrs)

        if self.getParameter('includeProxyForArchive').getValue(frameTime):
            sscb.addSubOpAtLocation(location,
                    'UsdIn.AddViewerProxy', attrs)

        interface.appendOp('StaticSceneCreate', sscb.build())
        return

    graphState = interface.getGraphState()
    usdInArgs = self.buildUsdInOpArgsAtGraphState(graphState)

    # When buildOpChain is reached as result of a call to
    # buildUsdInOpArgsFromDownstreamNode, an additional entry will be
    # present in the graphState (otherwise not meaningful to the cooked scene).
    # If found, we'll record opArgs at the specified key in a member variable
    # dict.
    argsCookTmpKey = graphState.getDynamicEntry(kArgsCookTmpKeyToken)
    if isinstance(argsCookTmpKey, FnAttribute.StringAttribute):
        self._argsCookTmp[argsCookTmpKey.getValue('', False)] = usdInArgs


    # our primary op in the chain that will create the root location
    sscb = FnGeolibServices.OpArgsBuilders.StaticSceneCreate(True)

    sscb.addSubOpAtLocation(self.getScenegraphLocation(
        interface.getFrameTime()), 'UsdIn', usdInArgs)
    sscb.addSubOpAtLocation(
        '/root/world', 'UsdIn.UpdateGlobalLists', usdInArgs)

    sscb.setAttrAtLocation('/root', 'info.usdLoader', FnAttribute.StringAttribute('UsdIn'))

    interface.appendOp('StaticSceneCreate', sscb.build())

nb.setGetScenegraphLocationFnc(getScenegraphLocation)
nb.setBuildOpChainFnc(buildOpChain)


#-----------------------------------------------------------------------------

# XXX prePopulate exists in some production data with an incorrect default
#     value. Assume all studio uses of it prior to this fix intend for
#     it to be enabled.
def usdInUpgradeToVersionTwo(nodeElement):
    prePopulateElement = NodegraphAPI.Xio.Node_getParameter(
            nodeElement, 'prePopulate')
    if prePopulateElement:
        NodegraphAPI.Xio.Parameter_setValue(prePopulateElement, 1)

nb.setNodeTypeVersion(2)
nb.setNodeTypeVersionUpdateFnc(2, usdInUpgradeToVersionTwo)







nb.build()


#-----------------------------------------------------------------------------

nb = Nodes3DAPI.NodeTypeBuilder('UsdInVariantSelect')
nb.setInputPortNames(("in",))

nb.setParametersTemplateAttr(FnAttribute.GroupBuilder()
    .set('location', '')
    .set('args.variantSetName', '')
    .set('args.variantSelection', '')
    .set('additionalLocations', FnAttribute.StringAttribute([]))
    .build())

nb.setHintsForParameter('location', {
    'widget' : 'scenegraphLocation',
})

nb.setHintsForParameter('additionalLocations', {
    'widget' : 'scenegraphLocationArray',
})

nb.setGenericAssignRoots('args', '__variantUI')

def getInputPortAndGraphState(self, outputPort, graphState):
    frameTime = graphState.getTime()

    location = self.getParameter("location").getValue(frameTime)

    variantSetName = ''
    if self.getParameter("args.variantSetName.enable").getValue(frameTime):
        variantSetName = str(self.getParameter("args.variantSetName.value")
                             .getValue(frameTime))

    variantSelection = None
    if self.getParameter("args.variantSelection.enable").getValue(frameTime):
         variantSelection = self.getParameter(
                "args.variantSelection.value").getValue(frameTime)

    if location and variantSetName and variantSelection is not None:
        entryName = FnAttribute.DelimiterEncode(str(location))
        entryPath = "variants." + entryName + "." + str(variantSetName)

        valueAttr = FnAttribute.StringAttribute(variantSelection)
        gb = FnAttribute.GroupBuilder()
        gb.set(entryPath, valueAttr)

        for addLocParam in self.getParameter(
                'additionalLocations').getChildren():
            location = addLocParam.getValue(frameTime)
            if location:
                entryName = FnAttribute.DelimiterEncode(str(location))
                entryPath = "variants." + entryName + "." + variantSetName
                gb.set(entryPath, valueAttr)


        existingValue = (
            graphState.getDynamicEntry("var:pxrUsdInSession"))

        if isinstance(existingValue, FnAttribute.GroupAttribute):
            gb.deepUpdate(existingValue)

        graphState = (graphState.edit()
                .setDynamicEntry("var:pxrUsdInSession", gb.build())
                .build())

    return Nodes3DAPI.Node3D.getInputPortAndGraphState(
        self, outputPort, graphState)

nb.setGetInputPortAndGraphStateFnc(getInputPortAndGraphState)

def getScenegraphLocation(self, frameTime):
    location = self.getParameter('location').getValue(frameTime)
    if not (location == '/root' or location.startswith('/root/')):
        location = '/root'
    return location

nb.setGetScenegraphLocationFnc(getScenegraphLocation)


def appendToParametersOpChain(self, interface):    
    frameTime = interface.getFrameTime()

    location = self.getScenegraphLocation(frameTime)
    variantSetName = ''
    if self.getParameter('args.variantSetName.enable').getValue(frameTime):
        variantSetName = self.getParameter(
                'args.variantSetName.value').getValue(frameTime)

    # This makes use of the attrs recognized by UsdInUtilExtraHintsDap
    # to provide the hinting from incoming attr values.
    uiscript = '''
        local variantSetName = Interface.GetOpArg('user.variantSetName'):getValue()

        local variantsGroup = (Interface.GetAttr('info.usd.variants') or
                GroupAttribute())

        local variantSetNames = {}
        for i = 0, variantsGroup:getNumberOfChildren() - 1 do
            variantSetNames[#variantSetNames + 1] = variantsGroup:getChildName(i)
        end

        Interface.SetAttr("__usdInExtraHints." ..
                Attribute.DelimiterEncode("__variantUI.variantSetName"),
                        GroupBuilder()
                            :set('widget', StringAttribute('popup'))
                            :set('options', StringAttribute(variantSetNames))
                            :set('editable', IntAttribute(1))
                            :build())

        local variantOptions = {}

        if variantSetName ~= '' then
            local variantOptionsAttr =
                    variantsGroup:getChildByName(variantSetName)
            if Attribute.IsString(variantOptionsAttr) then
                variantOptions = variantOptionsAttr:getNearestSample(0.0)
            end
        end

        Interface.SetAttr("__usdInExtraHints." ..
                Attribute.DelimiterEncode("__variantUI.variantSelection"),
                        GroupBuilder()
                            :set('widget', StringAttribute('popup'))
                            :set('options', StringAttribute(variantOptions))
                            :set('editable', IntAttribute(1))
                            :build())
    '''

    sscb = FnGeolibServices.OpArgsBuilders.StaticSceneCreate(True)

    sscb.addSubOpAtLocation(location, 'OpScript.Lua',
            FnAttribute.GroupBuilder()
                .set('script', uiscript)
                .set('user.variantSetName', variantSetName)
                .build())

    interface.appendOp('StaticSceneCreate', sscb.build())


nb.setAppendToParametersOpChainFnc(appendToParametersOpChain)

nb.build()

#-----------------------------------------------------------------------------

nb = Nodes3DAPI.NodeTypeBuilder('UsdInDefaultMotionSamples')
nb.setInputPortNames(("in",))

nb.setParametersTemplateAttr(FnAttribute.GroupBuilder()
    .set('locations', '')
    .build(),
        forceArrayNames=('locations',))

nb.setHintsForParameter('locations', {
    'widget' : 'scenegraphLocationArray',
    'help' : 'Hierarchy root location paths for which to use default motion sample times.'
})

def getInputPortAndGraphState(self, outputPort, graphState):
    frameTime = graphState.getTime()
    locations = self.getParameter("locations")

    if locations:
        gb = FnAttribute.GroupBuilder()

        for loc in locations.getChildren():
            gb.set("overrides." + FnAttribute.DelimiterEncode(
                    str(loc.getValue(frameTime))) + ".motionSampleTimes",
                    FnAttribute.IntAttribute(1))

        existingValue = (
            graphState.getDynamicEntry("var:pxrUsdInSession"))

        if isinstance(existingValue, FnAttribute.GroupAttribute):
            gb.deepUpdate(existingValue)

        graphState = (graphState.edit()
                .setDynamicEntry("var:pxrUsdInSession", gb.build())
                .build())

    return Nodes3DAPI.Node3D.getInputPortAndGraphState(
        self, outputPort, graphState)

nb.setGetInputPortAndGraphStateFnc(getInputPortAndGraphState)

nb.build()

#-----------------------------------------------------------------------------

nb = Nodes3DAPI.NodeTypeBuilder('UsdInMotionOverrides')
nb.setInputPortNames(('in',))

nb.setParametersTemplateAttr(FnAttribute.GroupBuilder()
    .set('locations', '')
    .set('overrides.motionSampleTimes', '')
    .set('overrides.currentTime', '')
    .set('overrides.shutterOpen', '')
    .set('overrides.shutterClose', '')
    .build(),
        forceArrayNames=('locations',))

nb.setHintsForParameter('locations', {
    'widget': 'scenegraphLocationArray',
    'help': 'Hierarchy root location paths for which overrides will be applied.'
})

nb.setHintsForParameter('overrides', {
    'help': 'Any non-empty overrides will be used for motion calculations.',
    'open': True,
})

def getInputPortAndGraphState(self, outputPort, graphState):
    frameTime = graphState.getTime()

    locations = self.getParameter('locations')
    overrides = self.getParameter('overrides')

    if locations.getNumChildren() > 0:

        # Build overrides as a child group
        gb1 = FnAttribute.GroupBuilder()

        motionSampleTimes = overrides.getChild('motionSampleTimes').getValue(frameTime)
        currentTime = overrides.getChild('currentTime').getValue(frameTime)
        shutterOpen = overrides.getChild('shutterOpen').getValue(frameTime)
        shutterClose = overrides.getChild('shutterClose').getValue(frameTime)

        if motionSampleTimes:
            floatTimes = [float(t) for t in motionSampleTimes.split(' ')]
            gb1.set('motionSampleTimes', FnAttribute.FloatAttribute(floatTimes))

        if currentTime:
            gb1.set('currentTime', FnAttribute.FloatAttribute(float(currentTime)))

        if shutterOpen:
            gb1.set('shutterOpen', FnAttribute.FloatAttribute(float(shutterOpen)))

        if shutterClose:
            gb1.set('shutterClose', FnAttribute.FloatAttribute(float(shutterClose)))

        overridesAttr = gb1.build()

        if overridesAttr.getNumberOfChildren() > 0:

            # Encode per-location overrides in the graph state
            gb2 = FnAttribute.GroupBuilder()

            for loc in locations.getChildren():
                encodedLoc = FnAttribute.DelimiterEncode(
                    str(loc.getValue(frameTime)))
                if encodedLoc:
                    gb2.set('overrides.' + encodedLoc, overridesAttr)

            existingValue = (
                graphState.getDynamicEntry('var:pxrUsdInSession'))
            if isinstance(existingValue, FnAttribute.GroupAttribute):
                gb2.deepUpdate(existingValue)

            graphState = (graphState.edit()
                    .setDynamicEntry('var:pxrUsdInSession', gb2.build())
                    .build())

    return Nodes3DAPI.Node3D.getInputPortAndGraphState(
        self, outputPort, graphState)

nb.setGetInputPortAndGraphStateFnc(getInputPortAndGraphState)

nb.build()

#-----------------------------------------------------------------------------

nb = Nodes3DAPI.NodeTypeBuilder('UsdInActivationSet')
nb.setInputPortNames(("in",))

nb.setParametersTemplateAttr(FnAttribute.GroupBuilder()
    .set('locations', '')
    .set('active', 0)
    .build(),
        forceArrayNames=('locations',))

nb.setHintsForParameter('locations', {
    'widget' : 'scenegraphLocationArray',
    'help' : 'locations to activate or deactivate.'
})

nb.setHintsForParameter('active', {
    'widget' : 'boolean',
})

def getInputPortAndGraphState(self, outputPort, graphState):
    frameTime = graphState.getTime()
    locations = self.getParameter('locations')

    if locations:
        state = FnAttribute.IntAttribute(
            bool(self.getParameter("active").getValue(frameTime)))

        gb = FnAttribute.GroupBuilder()

        for loc in locations.getChildren():
            gb.set("activations." + FnAttribute.DelimiterEncode(
                    str(loc.getValue(frameTime))), state)

        existingValue = (
            graphState.getDynamicEntry("var:pxrUsdInSession"))

        if isinstance(existingValue, FnAttribute.GroupAttribute):
            gb.deepUpdate(existingValue)

        graphState = (graphState.edit()
                .setDynamicEntry("var:pxrUsdInSession", gb.build())
                .build())

    return Nodes3DAPI.Node3D.getInputPortAndGraphState(
        self, outputPort, graphState)

nb.setGetInputPortAndGraphStateFnc(getInputPortAndGraphState)

nb.build()


#-----------------------------------------------------------------------------

nb = Nodes3DAPI.NodeTypeBuilder('UsdInAttributeSet')

nb.setInputPortNames(("in",))

nb.setParametersTemplateAttr(FnAttribute.GroupBuilder()
    .set('locations', '')
    .set('attrName', 'attr')
    .set('type', 'float')

    .set('asMetadata', 0)
    .set('listOpType', 'explicit')

    .set('numberValue', 1.0)
    .set('stringValue', '')

    .set('forceArrayForSingleValue', 0)

    .build(),
        forceArrayNames=(
            'locations',
            'numberValue',
            'stringValue'))

nb.setHintsForParameter('locations', {
    'widget' : 'scenegraphLocationArray',
    'help' : 'locations on which to set.'
})

nb.setHintsForParameter('type', {
    'widget' : 'popup',
    'options' : ['int', 'float', 'double', 'string', 'listOp',],
})

nb.setHintsForParameter('asMetadata', {
    'widget' : 'boolean',
})

nb.setHintsForParameter('forceArrayForSingleValue', {
    'widget' : 'boolean',
})

nb.setHintsForParameter('listOpType', {
    'widget' : 'popup',
    'options' : [
        'explicit',
        'added',
        'deleted',
        'ordered',
        'prepended',
        'appended'],
    'conditionalVisOp' : 'equalTo',
    'conditionalVisPath' : '../type',
    'conditionalVisValue' : 'listOp',
})

nb.setHintsForParameter('numberValue', {
    'widget' : 'sortableArray',
    'conditionalVisOp' : 'notEqualTo',
    'conditionalVisPath' : '../type',
    'conditionalVisValue' : 'string',
})

nb.setHintsForParameter('stringValue', {
    'widget' : 'sortableArray',
    'conditionalVisOp' : 'equalTo',
    'conditionalVisPath' : '../type',
    'conditionalVisValue' : 'string',
})

__numberAttrTypes = {
    'int' : FnAttribute.IntAttribute,
    'float' : FnAttribute.FloatAttribute,
    'double': FnAttribute.DoubleAttribute,
    'listOp': FnAttribute.IntAttribute,
}

def getInputPortAndGraphState(self, outputPort, graphState):
    frameTime = graphState.getTime()
    locationsParam = self.getParameter("locations")

    attrName = self.getParameter('attrName').getValue(
            frameTime).replace('.', ':')

    locations = [y for y in
        (x.getValue(frameTime) for x in locationsParam.getChildren()) if y]

    if attrName and locations:
        typeValue = self.getParameter('type').getValue(frameTime)
        if typeValue == 'string':
            valueAttr = NodegraphAPI.BuildAttrFromStringParameter(
                self.getParameter('stringValue'), graphState, False)
        else:
            valueAttr = NodegraphAPI.BuildAttrFromNumberParameter(
                self.getParameter('numberValue'),
                graphState,
                False,
                __numberAttrTypes.get(typeValue,
                    FnAttribute.FloatAttribute))

        if typeValue == 'listOp':
            entryGb = FnAttribute.GroupBuilder()
            entryGb.set('type', 'SdfInt64ListOp')
            entryGb.set('listOp.%s' % self.getParameter(
                    'listOpType').getValue(frameTime), valueAttr)

            entryGroup = entryGb.build()
        else:
            entryGb = FnAttribute.GroupBuilder()
            entryGb.set('value', valueAttr)

            if valueAttr.getNumberOfValues() == 1:
                if self.getParameter('forceArrayForSingleValue'
                        ).getValue(frameTime):
                    entryGb.set('forceArray', 1)

            entryGroup = entryGb.build()

        gb = FnAttribute.GroupBuilder()

        asMetadata = (typeValue == 'listOp'
                or self.getParameter('asMetadata').getValue(frameTime) != 0)

        for loc in locations:

            if asMetadata:

                if typeValue == 'listOp':
                    gb.set("metadata.%s.prim.%s" % (
                        FnAttribute.DelimiterEncode(str(loc)), attrName,),
                        entryGroup)


                # TODO, only listOps are supported at the moment.

            else:
                gb.set("attrs.%s.%s" % (
                        FnAttribute.DelimiterEncode(str(loc)), attrName,),
                        entryGroup)

        existingValue = (
            graphState.getDynamicEntry("var:pxrUsdInSession"))

        if isinstance(existingValue, FnAttribute.GroupAttribute):
            gb.deepUpdate(existingValue)

        graphState = (graphState.edit()
                .setDynamicEntry("var:pxrUsdInSession", gb.build())
                .build())

    return Nodes3DAPI.Node3D.getInputPortAndGraphState(
        self, outputPort, graphState)

nb.setGetInputPortAndGraphStateFnc(getInputPortAndGraphState)

nb.build()

#-----------------------------------------------------------------------------

nb = Nodes3DAPI.NodeTypeBuilder('UsdInIsolate')

nb.setInputPortNames(("in",))


nb.setParametersTemplateAttr(FnAttribute.GroupBuilder()
    .set('locations', '')
    .set('mode', 'append')
    .build(),
        forceArrayNames=(
            'locations',
            ))

nb.setHintsForParameter('locations', {
    'widget' : 'scenegraphLocationArray',
})

nb.setHintsForParameter('mode', {
    'widget' : 'popup',
    'options' : ['append', 'replace'],
})

def getInputPortAndGraphState(self, outputPort, graphState):
    frameTime = graphState.getTime()

    locationsParam = self.getParameter("locations")

    locations = [y for y in
        (x.getValue(frameTime) for x in locationsParam.getChildren()) if y]

    if locations:
        existingValue = (
            graphState.getDynamicEntry("var:pxrUsdInSession")
                    or FnAttribute.GroupAttribute())

        # later nodes set to 'replace' win out
        maskIsFinal = existingValue.getChildByName('maskIsFinal')
        if not maskIsFinal:

            gb = FnAttribute.GroupBuilder()

            gb.update(existingValue)

            mode = self.getParameter('mode').getValue(frameTime)

            if mode == 'replace':
                gb.set('mask', FnAttribute.StringAttribute(locations))
                gb.set('maskIsFinal', 1)
            else:
                existingLocationsAttr = existingValue.getChildByName('mask')
                if isinstance(existingLocationsAttr, FnAttribute.StringAttribute):
                    locations.extend(existingLocationsAttr.getNearestSample(0))

                gb.set('mask', FnAttribute.StringAttribute(locations))

            graphState = (graphState.edit()
                .setDynamicEntry("var:pxrUsdInSession", gb.build())
                .build())

    return Nodes3DAPI.Node3D.getInputPortAndGraphState(
        self, outputPort, graphState)

nb.setGetInputPortAndGraphStateFnc(getInputPortAndGraphState)

nb.build()

#-----------------------------------------------------------------------------

nb = Nodes3DAPI.NodeTypeBuilder('UsdInResolveMaterialBindings')

nb.setInputPortNames(("in",))

nb.setParametersTemplateAttr(FnAttribute.GroupBuilder()
    .set('purpose', '')
    .set('omitIfParentValueMatches', 0)
    .build())

nb.setHintsForParameter('purpose', {
    'help' : """
    The name of the purpose from which to tranfer material bindings to the 
    "materialAssign" attribute. An empty value is treated as "allPurpose". The
    sources of these values are within the "usd.materialBindings.*" attribute
    -- which will be present if "usePurposeBasedMaterialBinding" is enabled
    on the upstream UsdIn.
    """,
})

nb.setHintsForParameter('omitIfParentValueMatches', {
    'widget' : 'boolean',
    'help' : """If enabled, bindings will be omitted if they are identical
    to that of their parent. This is useful because collection-based bindings
    in USD default to apply to all prims at and under the specified paths" 
    """,
})

def buildOpChain(self, interface):
    interface.appendOp("UsdInResolveMaterialBindings",
            FnAttribute.GroupBuilder()
            .set("purpose", interface.buildAttrFromParam(
                    self.getParameter('purpose')))
            .set("omitIfParentValueMatches", interface.buildAttrFromParam(
                    self.getParameter('omitIfParentValueMatches'),
                            numberType=FnAttribute.IntAttribute))
            .build())

nb.setBuildOpChainFnc(buildOpChain)
nb.build()

NodegraphAPI.AddNodeFlavor('UsdIn', '3d')
NodegraphAPI.AddNodeFlavor('UsdInVariantSelect', '3d')
NodegraphAPI.AddNodeFlavor('UsdInVariantSelect', '3d')
NodegraphAPI.AddNodeFlavor('UsdInDefaultMotionSamples', '3d')
NodegraphAPI.AddNodeFlavor('UsdInMotionOverrides', '3d')
NodegraphAPI.AddNodeFlavor('UsdInActivationSet', '3d')
NodegraphAPI.AddNodeFlavor('UsdInAttributeSet', '3d')
NodegraphAPI.AddNodeFlavor('UsdInIsolate', '3d')
NodegraphAPI.AddNodeFlavor('UsdInResolveMaterialBindings', '3d')
NodegraphAPI.AddNodeFlavor('UsdIn', 'usdin')
NodegraphAPI.AddNodeFlavor('UsdInVariantSelect', 'usdin')
NodegraphAPI.AddNodeFlavor('UsdInVariantSelect', 'usdin')
NodegraphAPI.AddNodeFlavor('UsdInDefaultMotionSamples', 'usdin')
NodegraphAPI.AddNodeFlavor('UsdInMotionOverrides', 'usdin')
NodegraphAPI.AddNodeFlavor('UsdInActivationSet', 'usdin')
NodegraphAPI.AddNodeFlavor('UsdInAttributeSet', 'usdin')
NodegraphAPI.AddNodeFlavor('UsdInIsolate', 'usdin')
NodegraphAPI.AddNodeFlavor('UsdInResolveMaterialBindings', 'usdin')

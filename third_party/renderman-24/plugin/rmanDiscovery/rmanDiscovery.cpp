//
// Copyright 2019 Pixar
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

#include "pxr/base/arch/fileSystem.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/plug/registry.h"
#include "pxr/base/plug/plugin.h"
#include "rmanDiscovery.h"
#include "rmanArgsParser/rmanArgsParser.h"
#include "pxr/usd/ndr/filesystemDiscoveryHelpers.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_DEFINE_PRIVATE_TOKENS(
    _allowedExtensionTokens,
    (args)
    (oso)
    // The sdraliases extension is only used for the aliases files. Any results
    // discovered with this extension will be filtered out of the final results.
    (sdraliases)
);

NDR_REGISTER_DISCOVERY_PLUGIN(RmanDiscoveryPlugin)

static NdrStringVec computeDefaultSearchPaths()
{
    NdrStringVec searchPaths;

    // RMAN_SHADERPATH contains OSL (.oso)
    std::string shaderpath = TfGetenv("RMAN_SHADERPATH");
    if (!shaderpath.empty()) {
        NdrStringVec paths = TfStringSplit(shaderpath, ARCH_PATH_LIST_SEP);
        for (std::string const& path : paths)
            searchPaths.push_back(path);
    } else {
        // Default RenderMan installation under '$RMANTREE/lib/shaders'
        std::string rmantree = TfGetenv("RMANTREE");
        if (!rmantree.empty()) {
            searchPaths.push_back(TfStringCatPaths(rmantree, "lib/shaders"));
        }
        // Default hdPrman installation under 'plugins/usd/resources/shaders'
        PlugPluginPtr plugin =
            PlugRegistry::GetInstance().GetPluginWithName("hdPrmanLoader");
        if (plugin) {
            std::string path = TfGetPathName(plugin->GetPath());
            if (!path.empty()) {
                searchPaths.push_back(
                    TfStringCatPaths(path, "resources/shaders"));
            }
        }
    }
    // RMAN_RIXPLUGINPATH contains Args (.args) metadata
    std::string rixpluginpath = TfGetenv("RMAN_RIXPLUGINPATH");
    if (!rixpluginpath.empty()) {
        // Assume that args files are under an 'Args' directory
        NdrStringVec paths = TfStringSplit(rixpluginpath, ARCH_PATH_LIST_SEP);
        for (std::string const& path : paths)
            searchPaths.push_back(TfStringCatPaths(path, "Args"));
    } else {
        // Default RenderMan installation under '$RMANTREE/lib/plugins/Args'
        std::string rmantree = TfGetenv("RMANTREE");
        if (!rmantree.empty()) {
            searchPaths.push_back(
                TfStringCatPaths(rmantree, "lib/plugins/Args"));
        }
    }
    return searchPaths;
}

static NdrStringVec &
RmanDiscoveryPlugin_GetDefaultSearchPaths()
{
    static NdrStringVec defaultSearchPaths = computeDefaultSearchPaths();
    return defaultSearchPaths;
}

void
RmanDiscoveryPlugin_SetDefaultSearchPaths(const NdrStringVec &paths)
{
    RmanDiscoveryPlugin_GetDefaultSearchPaths() = paths;
}

static bool &
RmanDiscoveryPlugin_GetDefaultFollowSymlinks()
{
    static bool defaultFollowSymlinks = true;
    return defaultFollowSymlinks;
}

void
RmanDiscoveryPlugin_SetDefaultFollowSymlinks(bool followSymlinks)
{
    RmanDiscoveryPlugin_GetDefaultFollowSymlinks() = followSymlinks;
}

RmanDiscoveryPlugin::RmanDiscoveryPlugin()
{
    _searchPaths = RmanDiscoveryPlugin_GetDefaultSearchPaths();
    _allowedExtensions = TfToStringVector(_allowedExtensionTokens->allTokens);
    _followSymlinks = RmanDiscoveryPlugin_GetDefaultFollowSymlinks();
}

RmanDiscoveryPlugin::RmanDiscoveryPlugin(Filter filter)
    : RmanDiscoveryPlugin()
{
    _filter = std::move(filter);
}

RmanDiscoveryPlugin::~RmanDiscoveryPlugin() = default;

// Helper for getting the aliases from the special ".sdraliases" files if 
// found during discovery. These files are immediately parsed to extract aliases
// that may be applied to the other discovery results via the alias map. 
// Returns true if the discovery result is an aliases file as we want to filter
// it out of the final discovery results.
static bool
_GetAliasesFromAliasesDiscoveryResult(
    const NdrNodeDiscoveryResult &dr, 
    std::map<NdrIdentifier, NdrTokenVec> *aliasMap)
{
    static const std::string aliasExtension = 
        std::string(".") + _allowedExtensionTokens->sdraliases.GetString();
    if (TfStringEndsWith(dr.uri, aliasExtension)) {
        RmanArgsParserPlugin::ParseShaderAliases(dr, aliasMap);
        return true;
    }
    return false;
};

NdrNodeDiscoveryResultVec
RmanDiscoveryPlugin::DiscoverNodes(const Context& context)
{
    auto result = NdrFsHelpersDiscoverNodes(
        _searchPaths, _allowedExtensions, _followSymlinks, &context
    );

    std::map<NdrIdentifier, NdrTokenVec> aliasMap;

    // Filter predicate includes filtering out the aliases args as well
    // as anything failing the filter function if present. This has the side 
    // effect of populating the identifier to aliases map along with any side 
    // effects of the filter
    // function itself.
    auto filterPredicate = [&](NdrNodeDiscoveryResult &dr) {
        if (_GetAliasesFromAliasesDiscoveryResult(dr, &aliasMap)) {
            return true;
        }
        if (this->_filter) {
            return !this->_filter(dr);
        }
        return false;
    };

    // Filter results and populate aliases.
    result.erase(std::remove_if(result.begin(), result.end(), filterPredicate), 
                 result.end());

    // If we found aliases, go back and add them to the appropriate results.
    if (!aliasMap.empty()) {
        for (NdrNodeDiscoveryResult &dr : result) {
            const auto it = aliasMap.find(dr.identifier);
            if (it != aliasMap.end()) {
                dr.aliases = it->second;
            }
        }
    }

    return result;
}

const NdrStringVec& 
RmanDiscoveryPlugin::GetSearchURIs() const
{
    return _searchPaths;
}

PXR_NAMESPACE_CLOSE_SCOPE

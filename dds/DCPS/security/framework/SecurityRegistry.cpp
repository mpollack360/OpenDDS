/*
 *
 *
 * Distributed under the OpenDDS License.
 * See: http://www.opendds.org/license.html
 */

#include "DCPS/DdsDcps_pch.h"
#include "SecurityRegistry.h"
#include "SecurityConfig.h"
#include "dds/DCPS/transport/framework/EntryExit.h"
#include "dds/DCPS/Util.h"
#include "dds/DCPS/Service_Participant.h"
#include "dds/DCPS/EntityImpl.h"
#include "dds/DCPS/ConfigUtils.h"
#include "dds/DCPS/SafetyProfileStreams.h"

#include "ace/Singleton.h"
#include "ace/OS_NS_strings.h"
#include "ace/Service_Config.h"

OPENDDS_BEGIN_VERSIONED_NAMESPACE_DECL

namespace OpenDDS {
namespace DCPS {

#define NULL_FACTORY_PTR SecurityPluginFactory_rch()

const OPENDDS_STRING SecurityRegistry::DEFAULT_CONFIG_NAME(ACE_TEXT("_OPENDDS_DEFAULT_CONFIG"));
const OPENDDS_STRING SecurityRegistry::DEFAULT_INST_PREFIX(ACE_TEXT("_OPENDDS_"));
const OPENDDS_STRING SecurityRegistry::DEFAULT_PLUGIN_NAME(ACE_TEXT("BuiltIn"));
const OPENDDS_STRING SecurityRegistry::SECURITY_SECTION_NAME(ACE_TEXT("security"));
const OPENDDS_STRING SecurityRegistry::ACCESS_CTRL_PLUGIN_NAME(ACE_TEXT("access_ctrl_plugin"));
const OPENDDS_STRING SecurityRegistry::AUTHENTICATION_PLUGIN_NAME(ACE_TEXT("auth_plugin"));
const OPENDDS_STRING SecurityRegistry::CRYPTO_PLUGIN_NAME(ACE_TEXT("crypto_plugin"));


SecurityRegistry::SecurityConfigEntry::SecurityConfigEntry(const OPENDDS_STRING& entryName)
	: entry_name_(entryName)
	, auth_name_(DEFAULT_PLUGIN_NAME)
	, access_ctrl_name_(DEFAULT_PLUGIN_NAME)
	, crypto_name_(DEFAULT_PLUGIN_NAME)
{
}

SecurityRegistry::~SecurityRegistry()
{
  DBG_ENTRY_LVL("SecurityRegistry", "~SecurityRegistry", 6);
  release();
}

SecurityRegistry::SecurityConfigEntry::~SecurityConfigEntry()
{
}

void
SecurityRegistry::SecurityConfigEntry::add_property(const OPENDDS_STRING& name, const OPENDDS_STRING& value)
{
	// Move these up
	static const OPENDDS_STRING AUTH_CONFIG_PROP_NAME("auth_config");
	static const OPENDDS_STRING ACCESS_CTRL_CONFIG_PROP_NAME("access_ctrl_config");
	static const OPENDDS_STRING CRYPTO_CONFIG_PROP_NAME("crypto_config");

	// Config properties can either identity a specific plugin, or
	// a configuration property for the security plugins
	// TODO - External plugins are not enable yet
	//if (0 == name.compare(AUTH_CONFIG_PROP_NAME)) {
	//	auth_config_name_ = name;
	//} else if (0 == name.compare(ACCESS_CTRL_CONFIG_PROP_NAME)) {
	//	access_ctrl_name_ = name;
	//} else if (0 == name.compare(CRYPTO_CONFIG_PROP_NAME)) {
	//	crypto_name_ = name;
	//} else {
	properties_.push_back(std::make_pair(name, value));
	//}
}


SecurityRegistry*
SecurityRegistry::instance()
{
  return ACE_Unmanaged_Singleton<SecurityRegistry, ACE_Recursive_Thread_Mutex>::instance();
}

void
SecurityRegistry::close()
{
  ACE_Unmanaged_Singleton<SecurityRegistry, ACE_Recursive_Thread_Mutex>::close();
}


SecurityRegistry::SecurityRegistry()
{
  DBG_ENTRY_LVL("SecurityRegistry", "SecurityRegistry", 6);

  lib_directive_map_[DEFAULT_PLUGIN_NAME] = "dynamic OpenDDS_Security Service_Object * OpenDDS_Security:_make_BuiltInPluginLoader()";
}

void
SecurityRegistry::release()
{
  DBG_ENTRY_LVL("SecurityRegistry", "release", 6);
  GuardType guard(lock_);

  for (InstMap::iterator iter = registered_plugins_.begin(); iter != registered_plugins_.end(); ++iter) {
    iter->second->shutdown();
  }

  registered_plugins_.clear();
  config_map_.clear();

}

void
SecurityRegistry::register_plugin(const OPENDDS_STRING& plugin_name,
																	SecurityPluginInst_rch plugin)
{
	GuardType guard(lock_);

	if (registered_plugins_.find(plugin_name) != registered_plugins_.end()) {
		guard.release();
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) SecurityRegistry::register_plugin: ")
               ACE_TEXT("plugin=%s already exists.\n"),
               plugin_name.c_str()));
	} else {
		registered_plugins_.insert(std::make_pair(plugin_name, plugin));
	}
}

SecurityConfig_rch
SecurityRegistry::create_config(const OPENDDS_STRING& config_name)
{
	// If the configuration instance already exists, then it can be reused.
	// Otherwise create a new one and save it for any later needs
	SecurityConfig_rch existing_config;
	if (find_config(config_name, existing_config)) {
		return existing_config;
	}

	// This is making an assumption that the entry map is only written
	// to in single-threaded operation, and all acess from that point on
	// is read-only
	ConfigEntryMap::const_iterator iEntry = config_entries_.find(config_name);
	if (iEntry == config_entries_.end()) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) SecurityRegistry::create_config: ")
               ACE_TEXT("config=%s does not exist.\n"),
               config_name.c_str()));
		return SecurityConfig_rch();
	}

	// This will load any libraries that need to be loaded, and use the
	// resulting plugin instance objects to create the concrete implementations
	const SecurityConfigEntry_rch& entry = iEntry->second;
	SecurityPluginInst_rch auth_plugin_inst = get_plugin_inst(entry->get_auth_name());
	if (auth_plugin_inst.is_nil()) {
    ACE_ERROR((LM_ERROR,
               ACE_TEXT("(%P|%t) SecurityRegistry::create_config: ")
               ACE_TEXT("Failed to load authentication plugin %s\n"),
               entry->get_auth_name().c_str()));
		return SecurityConfig_rch();
	}

	SecurityPluginInst_rch ac_plugin_inst = get_plugin_inst(entry->get_access_control_name());
	if (auth_plugin_inst.is_nil()) {
		ACE_ERROR((LM_ERROR,
							ACE_TEXT("(%P|%t) SecurityRegistry::create_config: ")
							ACE_TEXT("Failed to load access control plugin %s\n"),
							entry->get_access_control_name().c_str()));
		return SecurityConfig_rch();
	}

	SecurityPluginInst_rch crypto_plugin_inst = get_plugin_inst(entry->get_crypto_name());
	if (auth_plugin_inst.is_nil()) {
		ACE_ERROR((LM_ERROR,
							ACE_TEXT("(%P|%t) SecurityRegistry::create_config: ")
							ACE_TEXT("Failed to load crypto plugin %s\n"),
							entry->get_crypto_name().c_str()));
		return SecurityConfig_rch();
	}

	// Create the new config and try to add it to the container
	// of existing configs.  If this fails for some reason, then
	// release the new config and fail
	SecurityConfig_rch new_config = make_rch<SecurityConfig>(
				config_name,
				auth_plugin_inst->create_authentication(),
				ac_plugin_inst->create_access_control(),
				crypto_plugin_inst->create_crypto_key_exchange(),
				crypto_plugin_inst->create_crypto_key_factory(),
				crypto_plugin_inst->create_crypto_transform(),
				entry->get_properties());

	if (!add_config(config_name, new_config)) {
		ACE_ERROR((LM_ERROR,
							ACE_TEXT("(%P|%t) SecurityRegistry::create_config: ")
							ACE_TEXT("Error storing config instance %s\n"),
							config_name.c_str()));
		new_config.reset();
	}

	return new_config;
}

int
SecurityRegistry::load_security_configuration(ACE_Configuration_Heap& cf)
{
  const ACE_Configuration_Section_Key& root = cf.root_section();
  ACE_TString sect_name;

  for (int index = 0; cf.enumerate_sections(root, index, sect_name) == 0; ++index) {
    if (ACE_OS::strcmp(sect_name.c_str(), SECURITY_SECTION_NAME.c_str()) == 0) {
      // found the section, now iterate through subsections...
      ACE_Configuration_Section_Key sect;
      if (cf.open_section(root, sect_name.c_str(), 0, sect) != 0) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("(%P|%t) SecurityRegistry::load_plugin_properties: ")
                          ACE_TEXT("failed to open section %s\n"),
                          sect_name.c_str()),
                         -1);
      }

			// Ensure there are no properties in this section
			ValueMap vm;
			if (pullValues(cf, sect, vm) > 0) {
				// There are values inside [transport]
				ACE_ERROR_RETURN((LM_ERROR,
													ACE_TEXT("(%P|%t) SecurityRegistry::load_plugin_properties: ")
													ACE_TEXT("[%s] sections must have a section name\n"),
													sect_name.c_str()),
												 -1);
			}
			// Process the subsections of this section
			KeyList keys;
			if (processSections(cf, sect, keys) != 0) {
				ACE_ERROR_RETURN((LM_ERROR,
													ACE_TEXT("(%P|%t) SecurityRegistry::load_plugin_properties: ")
													ACE_TEXT("too many nesting layers in [%s] section.\n"),
													sect_name.c_str()),
												 -1);
			}

			// Save the properties configured for each security entry
			for (KeyList::const_iterator it = keys.begin(); it != keys.end(); ++it) {
				const OPENDDS_STRING& entry_name = it->first;
				// Duplicate entry check
				if (config_entries_.find(entry_name) != config_entries_.end()) {
					ACE_ERROR_RETURN((LM_ERROR,
														ACE_TEXT("(%P|%t) SecurityRegistry::load_plugin_properties: ")
														ACE_TEXT("duplicate sections named [%s/%s].\n"),
														sect_name.c_str(), entry_name.c_str()),
													 -1);
				}

				// Copy any existing properties in the entry and create the SecurityConfigEntry, which
				// will be stored until actual plugin instances are needed for this configuration
				SecurityConfigEntry_rch newEntry = make_rch<SecurityConfigEntry>(it->first);
				ValueMap values;
				pullValues(cf, it->second, values);
				for (DCPS::ValueMap::const_iterator val = values.begin(); val != values.end(); ++val) {
					newEntry->add_property(val->first, val->second);
				}

				config_entries_[it->first] = newEntry;
			}
    }
  }

  return 0;
}

void
SecurityRegistry::load_security_plugin_lib(const OPENDDS_STRING& security_plugin_type)
{
  ACE_UNUSED_ARG(security_plugin_type);
#if !defined(ACE_AS_STATIC_LIBS)
  GuardType guard(lock_);
  LibDirectiveMap::iterator lib_iter = lib_directive_map_.find(security_plugin_type);
  if (lib_iter != lib_directive_map_.end()) {
    ACE_TString directive = ACE_TEXT_CHAR_TO_TCHAR(lib_iter->second.c_str());
    // Release the lock, because loading a transport library will
    // recursively call this function to add its default inst.
    guard.release();
    ACE_Service_Config::process_directive(directive.c_str());
  }
#endif
}

bool
SecurityRegistry::find_config(const OPENDDS_STRING& name, SecurityConfig_rch& config)
{
	GuardType guard(lock_);

	bool found_config = false;
	ConfigMap::iterator iConfig = config_map_.find(name);
	if (iConfig != config_map_.end()) {
		config = iConfig->second;
		found_config = true;
	}

	return found_config;
}

bool
SecurityRegistry::add_config(const OPENDDS_STRING& name, SecurityConfig_rch& config)
{
	GuardType guard(lock_);

	bool added_config = false;
	ConfigMap::iterator iConfig = config_map_.find(name);
	if (iConfig == config_map_.end()) {
		config_map_[name] = config;
		added_config = true;
	} else {
		// Someone else added this already. Use it instead
		config = iConfig->second;
		added_config = true;
	}

	return added_config;
}

SecurityPluginInst_rch SecurityRegistry::get_plugin_inst(const OPENDDS_STRING& plugin_name)
{
  GuardType guard(lock_);

  SecurityPluginInst_rch plugin_inst;

  if (find(registered_plugins_, plugin_name, plugin_inst) != 0) {
#if !defined(ACE_AS_STATIC_LIBS)
    guard.release();
    // Not present, try to load library
    load_security_plugin_lib(plugin_name);
    guard.acquire();

    // Try to find it again
    if (find(registered_plugins_, plugin_name, plugin_inst) != 0) {
#endif
      ACE_ERROR((LM_ERROR,
                 ACE_TEXT("(%P|%t) SecurityRegistry::create_inst: ")
                 ACE_TEXT("plugin_type=%C is not registered.\n"),
                 plugin_name.c_str()));
      return SecurityPluginInst_rch();
#if !defined(ACE_AS_STATIC_LIBS)
    }
#endif
  }

  return plugin_inst;
}


}
}

OPENDDS_END_VERSIONED_NAMESPACE_DECL
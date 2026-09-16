#include "register_types.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

#include "src/git_plugin.h"

// 本模块是编辑器侧的 VCS 插件（config.py 的 can_build 保证了只在 tools 构建下编译）。
void initialize_godot_git_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR) {
		return;
	}

	ClassDB::register_class<GitPlugin>();
}

void uninitialize_godot_git_module(ModuleInitializationLevel p_level) {
	// GitPlugin 由编辑器按需实例化，无全局单例需要释放。
}

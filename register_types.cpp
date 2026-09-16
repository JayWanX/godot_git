#include "register_types.h"

#include "core/config/project_settings.h"
#include "core/object/class_db.h"

#include "src/git.h"

// 本模块是编辑器侧的原生 VCS 实现（config.py 的 can_build 保证了只在 tools 构建下编译）。
void initialize_godot_git_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR) {
		return;
	}

	ClassDB::register_class<Git>();
}

void uninitialize_godot_git_module(ModuleInitializationLevel p_level) {
	// Git 由编辑器按需实例化，无全局单例需要释放。
}

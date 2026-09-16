def can_build(env, platform):
	# EditorVCSInterface 属于编辑器；仅 editor（tools）目标编译本模块，
	# 导出模板等非 editor 目标跳过。Godot 构建环境没有 "tools" 键。
	return env["target"] == "editor"


def configure(env):
	pass

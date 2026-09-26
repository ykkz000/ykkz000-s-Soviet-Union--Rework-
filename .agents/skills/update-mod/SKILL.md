---
name: update-mod
description: 更新模组内容的流程规范
---

1. 按计划/要求修改相应的源文件
2. 使用`.tools/build.ps1`编译项目，在未修改资源时可以使用`--skip-art`参数，若删除了文件则必须使用`--clean`参数清除原来的编译结果
3. 使用`git`暂存和提交修改
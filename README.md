# longshot-lua

为按键精灵 iOS 的 Lua 5.2 运行时构建 C/C++ 长截图拼接模块。

## 产物

- `longshot.so`：Lua C 模块，导出 `luaopen_longshot`
- `probe.so`：最小 Lua C 模块，先用来验证按键精灵是否能加载 C 模块

## 使用

把 `probe.so` 放到设备上 Lua `package.cpath` 能找到的位置，例如：

```text
/usr/local/lib/lua/5.2/probe.so
```

然后在 Lua/MQ 中测试：

```lua
local ok, mod = pcall(require, "probe")
print(ok, type(mod))
if ok then print(mod.ping()) end
```

`probe` 成功后，再把 `longshot.so` 放到：

```text
/usr/local/lib/lua/5.2/longshot.so
```

Lua 调用示例：

```lua
local longshot = require("longshot")
local result = longshot.stitch(
    {"/tmp/frame1.jpg", "/tmp/frame2.jpg"},
    {0, 1200},
    "/tmp/result.jpg",
    40,
    20
)
print(result)
```

`result` 成功时是输出路径，失败时以 `ERROR:` 开头。

## 重要限制

- 目标设备是 iOS arm64，Lua 5.2。
- `longshot.so` 通过 `-undefined dynamic_lookup` 依赖宿主导出 Lua C API。
- 必须先验证 `probe.so` 能加载；如果 `probe.so` 都加载失败，`longshot.so` 也不能用。
- 拼接算法复用原 `jpeg_stitcher.cpp` 和 libjpeg-turbo。
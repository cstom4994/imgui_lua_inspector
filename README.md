# ImGui Lua Inspector

ImGui tool for inspecting and modifying the Lua registry's value in real time which can embed in your game engine easily
[imgui_lua_inspector](https://github.com/cstom4994/imgui_lua_inspector)

## Usage
```cpp
// Just register luainspector functions to lua
lua_register(L, "__neko_luainspector_init", neko::luainspector::luainspector_init);
lua_register(L, "__neko_luainspector_draw", neko::luainspector::luainspector_draw);
lua_register(L, "__neko_luainspector_get", neko::luainspector::luainspector_get);

/*
__neko_luainspector_init should be called when your game is initialized, it will return a luainspector userdata
__neko_luainspector_draw should be called every frame (before ImGui::Render()), it needs to be called with a luainspector userdata
__neko_luainspector_get is used to get the userdata of the only luainspector that exists in the lua registry
*/
```

```lua

function game_init()
    inspector = __neko_luainspector_init()
end

function game_render()
    __neko_luainspector_draw(inspector)
    -- or __neko_luainspector_draw(__neko_luainspector_get())

    -- ...
    -- ImGui::Render() called
end

```

## Demo

![s1](demo.gif)

add_rules("mode.debug", "mode.release")

set_languages("c++20")

add_requires("imgui", {configs = {glfw_opengl3 = true}})
add_requires("lua")

target("example")
    set_kind("binary")
    add_headerfiles("imgui_lua_inspector.hpp")
    add_files("imgui_lua_inspector.cpp", "example/main.cpp")
    add_packages("lua", "imgui")


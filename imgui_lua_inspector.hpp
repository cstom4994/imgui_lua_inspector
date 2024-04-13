
#ifndef NEKO_LUA_INSPECTOR_HPP
#define NEKO_LUA_INSPECTOR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// You should include your lua and imgui here

// #include "engine/neko_lua.h"
// #include "sandbox/game_imgui.h"

namespace neko {

template <typename ForwardIterator, typename SpaceDetector>
constexpr ForwardIterator find_terminating_word(ForwardIterator begin, ForwardIterator end, SpaceDetector&& is_space_pred) {
    auto rend = std::reverse_iterator(begin);
    auto rbegin = std::reverse_iterator(end);

    int sp_size = 0;
    auto is_space = [&sp_size, &is_space_pred, &end](char c) {
        sp_size = is_space_pred(std::string_view{&c, static_cast<unsigned>(&*std::prev(end) - &c)});
        return sp_size > 0;
    };

    auto search = std::find_if(rbegin, rend, is_space);
    if (search == rend) {
        return begin;
    }
    ForwardIterator it = std::prev(search.base());
    it += sp_size;
    return it;
}

template <typename ForwardIt, typename OutputIt>
constexpr void copy(ForwardIt src_beg, ForwardIt src_end, OutputIt dest_beg, OutputIt dest_end) {
    while (src_beg != src_end && dest_beg != dest_end) {
        *dest_beg++ = *src_beg++;
    }
}

template <typename T>
T neko_lua_to(lua_State *L, int index) {
    if constexpr (std::same_as<T, int32_t> || std::same_as<T, uint32_t>) {
        luaL_argcheck(L, lua_isnumber(L, index), index, "number expected");
        return static_cast<T>(lua_tointeger(L, index));
    } else if constexpr (std::same_as<T, float> || std::same_as<T, double>) {
        luaL_argcheck(L, lua_isnumber(L, index), index, "number expected");
        return static_cast<T>(lua_tonumber(L, index));
    } else if constexpr (std::same_as<T, const char*>) {
        luaL_argcheck(L, lua_isstring(L, index), index, "string expected");
        return lua_tostring(L, index);
    } else if constexpr (std::same_as<T, bool>) {
        luaL_argcheck(L, lua_isboolean(L, index), index, "boolean expected");
        return lua_toboolean(L, index) != 0;
    } else if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(lua_topointer(L, index));
    } else {
        static_assert(std::is_same_v<T, void>, "Unsupported type for neko_lua_to");
    }
}

template <typename Iterable>
neko_inline bool neko_lua_equal(lua_State *state, const Iterable &indices) {
    auto it = indices.begin();
    auto end = indices.end();
    if (it == end) return true;
    int cmp_index = *it++;
    while (it != end) {
        int index = *it++;
        if (!neko_lua_equal(state, cmp_index, index)) return false;
        cmp_index = index;
    }
    return true;
}

inline bool incomplete_chunk_error(const char* err, std::size_t len) { return err && (std::strlen(err) >= 5u) && (0 == std::strcmp(err + len - 5u, "<eof>")); }

struct luainspector_hints {
    static std::string clean_table_list(const std::string& str);
    static bool try_replace_with_metaindex(lua_State* L);
    static bool collect_hints_recurse(lua_State* L, std::vector<std::string>& possible, const std::string& last, bool usehidden, unsigned left);
    static void prepare_hints(lua_State* L, std::string str, std::string& last);
    static bool collect_hints(lua_State* L, std::vector<std::string>& possible, const std::string& last, bool usehidden);
    static std::string common_prefix(const std::vector<std::string>& possible);
};

class luainspector;

struct command_line_input_callback_UserData {
    std::string* Str;
    ImGuiInputTextCallback ChainCallback;
    void* ChainCallbackUserData;
    neko::luainspector* luainspector_ptr;
};

struct inspect_table_config {
    const char* search_str = 0;
    bool is_non_function = false;
};

enum luainspector_logtype { LUACON_LOG_TYPE_WARNING = 1, LUACON_LOG_TYPE_ERROR = 2, LUACON_LOG_TYPE_NOTE = 4, LUACON_LOG_TYPE_SUCCESS = 0, LUACON_LOG_TYPE_MESSAGE = 3 };

class luainspector {
private:
    std::vector<std::pair<std::string, luainspector_logtype>> messageLog;

    lua_State* L;
    std::vector<std::string> m_history;
    int m_hindex;

    std::string cmd, cmd2;
    bool m_should_take_focus{false};
    ImGuiID m_input_text_id{0u};
    ImGuiID m_previously_active_id{0u};
    std::string_view m_autocomlete_separator{" | "};
    std::vector<std::string> m_current_autocomplete_strings{};

private:
    static int try_push_style(ImGuiCol col, const std::optional<ImVec4>& color) {
        if (color) {
            ImGui::PushStyleColor(col, *color);
            return 1;
        }
        return 0;
    }

public:
    void display(bool* textbox_react) noexcept;
    void print_line(const std::string& msg, luainspector_logtype type) noexcept;

    static luainspector* get_from_registry(lua_State* L);
    static void inspect_table(lua_State* L, inspect_table_config& cfg);
    static int luainspector_init(lua_State* L);
    static int luainspector_draw(lua_State* L);
    static int luainspector_get(lua_State* L);
    static int command_line_callback_st(ImGuiInputTextCallbackData* data) noexcept;

    void setL(lua_State* L);
    int command_line_input_callback(ImGuiInputTextCallbackData* data);
    bool command_line_input(const char* label, std::string* str, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* user_data = nullptr);
    void show_autocomplete() noexcept;
    std::string read_history(int change);
    std::string try_complete(std::string inputbuffer);
    void print_luastack(int first, int last, luainspector_logtype logtype);
    bool try_eval(std::string m_buffcmd, bool addreturn);
};
}  // namespace neko

#endif

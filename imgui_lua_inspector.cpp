#include "imgui_lua_inspector.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

static int __luainspector_echo(lua_State* L) {
    neko::luainspector* m = *static_cast<neko::luainspector**>(lua_touserdata(L, lua_upvalueindex(1)));
    if (m) m->print_line(luaL_checkstring(L, 1), neko::LUACON_LOG_TYPE_MESSAGE);
    return 0;
}

static int __luainspector_gc(lua_State* L) {
    neko::luainspector* m = *static_cast<neko::luainspector**>(lua_touserdata(L, 1));
    if (m) m->setL(0x0);
    return 0;
}

namespace neko {

std::string luainspector_hints::clean_table_list(const std::string& str) {
    std::string ret;
    bool got_dot = false, got_white = false;
    std::size_t whitespace_start = 0u;
    for (std::size_t i = 0u; i < str.size(); ++i) {
        const char c = str[i] == ':' ? '.' : str[i];
        if (!got_white && c == ' ') {
            got_white = true;
            whitespace_start = i;
        }
        if (c == '.' && got_white) {
            for (std::size_t j = 0u; j < (i - whitespace_start); ++j) ret.erase(--ret.end());
        }
        if (c != ' ') got_white = false;
        if (c != ' ' || !got_dot) ret += c;
        if (c == '.') got_dot = true;
        if (c != '.' && c != ' ') got_dot = false;
    }

    const std::string specials = "()[]{}\"'+-=/*^%#~,";
    for (std::size_t i = 0u; i < specials.size(); ++i) std::replace(ret.begin(), ret.end(), specials[i], ' ');

    ret = ret.substr(ret.find_last_of(' ') + 1u);
    return ret;
}

void luainspector_hints::prepare_hints(lua_State* L, std::string str, std::string& last) {
    str = clean_table_list(str);

    std::vector<std::string> tables;
    int begin = 0;
    for (std::size_t i = 0u; i < str.size(); ++i) {
        if (str[i] == '.') {
            tables.push_back(str.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    last = str.substr(begin);

    lua_pushglobaltable(L);
    for (std::size_t i = 0u; i < tables.size(); ++i) {
        if (lua_type(L, -1) != LUA_TTABLE) {
            lua_getmetatable(L, -1);
        }

        if (lua_type(L, -1) != LUA_TTABLE && !luaL_getmetafield(L, -1, "__index") && !lua_getmetatable(L, -1)) break;
        if (lua_type(L, -1) != LUA_TTABLE) break;  // no
        lua_pushlstring(L, tables[i].c_str(), tables[i].size());
        lua_gettable(L, -2);
    }
}

bool luainspector_hints::collect_hints_recurse(lua_State* L, std::vector<std::string>& possible, const std::string& last, bool usehidden, unsigned left) {
    if (left == 0u) return true;

    const bool skip_under_score = last.empty() && !usehidden;

    lua_pushnil(L);
    while (lua_next(L, -2)) {
        std::size_t keylen;
        const char* key;
        bool match = true;
        lua_pop(L, 1);
        lua_pushvalue(L, -1);  // for lua_next
        key = lua_tolstring(L, -1, &keylen);
        if (last.size() > keylen) {
            lua_pop(L, 1);
            continue;
        }
        for (std::size_t i = 0u; i < last.size(); ++i)
            if (key[i] != last[i]) match = false;
        if (match && (!skip_under_score || key[0] != '_')) possible.push_back(key);
        lua_pop(L, 1);  //
    }

    // Check whether the table itself has an index for linking elements
    if (luaL_getmetafield(L, -1, "__index")) {
        if (lua_istable(L, -1)) return collect_hints_recurse(L, possible, last, usehidden, left - 1);
        lua_pop(L, 1);  // pop
    }
    lua_pop(L, 1);  // pop table
    return true;
}

// Replace the value at the top of the stack with the __index TABLE from the metatable
bool luainspector_hints::try_replace_with_metaindex(lua_State* L) {
    if (!luaL_getmetafield(L, -1, "__index")) return false;

    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 2);  // pop value and key
        return false;
    }

    lua_insert(L, -2);  // move table under value
    lua_pop(L, 1);      // pop value
    return true;
}

bool luainspector_hints::collect_hints(lua_State* L, std::vector<std::string>& possible, const std::string& last, bool usehidden) {
    if (lua_type(L, -1) != LUA_TTABLE && !luainspector_hints::try_replace_with_metaindex(L)) return false;
    // table so just collect on it
    return collect_hints_recurse(L, possible, last, usehidden, 10u);
}

std::string luainspector_hints::common_prefix(const std::vector<std::string>& possible) {
    std::string ret;
    std::size_t maxindex = 1000000000u;
    for (std::size_t i = 0u; i < possible.size(); ++i) maxindex = std::min(maxindex, possible[i].size());
    for (std::size_t checking = 0u; checking < maxindex; ++checking) {
        const char c = possible[0u][checking];
        for (std::size_t i = 1u; i < possible.size(); ++i)
            if (c != possible[i][checking]) {
                checking = maxindex;
                break;
            }
        if (checking != maxindex) ret += c;
    }
    return ret;
}

}  // namespace neko

const char* const kMetaname = "__neko_lua_inspector_meta";

static void* __neko_lua_inspector_lightkey() {
    static char KEY;
    return &KEY;
}

static void* __neko_lua_inspector_print_func_lightkey() {
    static char KEY;
    return &KEY;
}

neko::luainspector* neko::luainspector::get_from_registry(lua_State* L) {
    lua_pushlightuserdata(L, __neko_lua_inspector_lightkey());  // # -1
    lua_gettable(L, LUA_REGISTRYINDEX);

    neko::luainspector* ret = nullptr;

    if (lua_type(L, -1) == LUA_TUSERDATA && lua_getmetatable(L, -1)) {
        // # -1 = metatable
        // # -2 = userdata
        lua_getfield(L, LUA_REGISTRYINDEX, kMetaname);  // get inspector metatable from registry
        // # -1 = metatable
        // # -2 = metatable
        // # -3 = userdata
        if (neko_lua_equal(L, -1, -2)) {                                      // determine is two metatable equal
            ret = *static_cast<neko::luainspector**>(lua_touserdata(L, -3));  // inspector userdata
        }

        lua_pop(L, 2);  // pop two
    }

    lua_pop(L, 1);  // pop inspector userdata
    return ret;
}

void neko::luainspector::print_luastack(int first, int last, luainspector_logtype logtype) {
    std::stringstream ss;
    for (int i = first; i <= last; ++i) {
        switch (lua_type(L, i)) {
            case LUA_TNUMBER:
                ss << lua_tostring(L, i);
                break;
            case LUA_TSTRING:
                ss << "'" << lua_tostring(L, i) << "'";
                break;
            case LUA_TBOOLEAN:
                ss << (lua_toboolean(L, i) ? "true" : "false");
                break;
            case LUA_TNIL:
                ss << "nil";
                break;
            default:
                ss << luaL_typename(L, i) << ": " << lua_topointer(L, i);
                break;
        }
        ss << ' ';
    }
    print_line(ss.str(), logtype);
}

bool neko::luainspector::try_eval(std::string m_buffcmd, bool addreturn) {
    if (addreturn) {
        const std::string code = "return " + m_buffcmd;
        if (LUA_OK == luaL_loadstring(L, code.c_str())) {
            return true;
        } else {
            lua_pop(L, 1);  // pop error
            return false;
        }
    } else {
        return LUA_OK == luaL_loadstring(L, m_buffcmd.c_str());
    }
}

// Avoid error when calling with non-strings
static inline std::string adjust_error_msg(lua_State* L, int idx) {
    const int t = lua_type(L, idx);
    if (t == LUA_TSTRING) return lua_tostring(L, idx);
    return std::string("(non string error value - ") + lua_typename(L, t) + ")";
}

void neko::luainspector::setL(lua_State* L) {
    this->L = L;

    if (!L) return;

    neko::luainspector** ptr = static_cast<neko::luainspector**>(lua_newuserdata(L, sizeof(neko::luainspector*)));
    (*ptr) = this;

    luaL_newmetatable(L, kMetaname);  // table
    lua_pushliteral(L, "__gc");
    lua_pushcfunction(L, &__luainspector_gc);
    lua_settable(L, -3);  // table[gc]=ConsoleModel_gc
    lua_setmetatable(L, -2);

    lua_pushlightuserdata(L, __neko_lua_inspector_lightkey());
    lua_pushvalue(L, -2);
    lua_settable(L, LUA_REGISTRYINDEX);

    lua_pushcclosure(L, &__luainspector_echo, 1);
    lua_setglobal(L, "echo");
}

std::string neko::luainspector::read_history(int change) {
    const bool was_promp = static_cast<std::size_t>(m_hindex) == m_history.size();

    m_hindex += change;
    m_hindex = std::max<int>(m_hindex, 0);
    m_hindex = std::min<int>(m_hindex, m_history.size());

    if (static_cast<std::size_t>(m_hindex) == m_history.size()) {
        return m_history[m_hindex - 1];
    } else {
        return m_history[m_hindex];
    }
}

std::string neko::luainspector::try_complete(std::string inputbuffer) {
    if (!L) {
        print_line("Lua state pointer is NULL, no completion available", LUACON_LOG_TYPE_ERROR);
        return inputbuffer;
    }

    std::vector<std::string> possible;  // possible match
    std::string last;

    const std::string lastbeg = inputbuffer;
    luainspector_hints::prepare_hints(L, lastbeg, last);
    if (!luainspector_hints::collect_hints(L, possible, last, false)) {
        lua_pushglobaltable(L);
        luainspector_hints::collect_hints(L, possible, last, false);
    }

    lua_settop(L, 0);  // Pop all

    if (possible.size() > 1u) {
        const std::string common_prefix = luainspector_hints::common_prefix(possible);
        if (common_prefix.empty() || common_prefix.size() <= last.size()) {
            std::string msg = possible[0];
            for (std::size_t i = 1u; i < possible.size(); ++i) msg += " " + possible[i];
            print_line(msg, LUACON_LOG_TYPE_NOTE);
            m_current_autocomplete_strings = possible;
        } else {
            const std::string added = common_prefix.substr(last.size());
            inputbuffer = lastbeg + added;
            m_current_autocomplete_strings.clear();
        }
    } else if (possible.size() == 1) {
        const std::string added = possible[0].substr(last.size());
        inputbuffer = lastbeg + added;
        m_current_autocomplete_strings.clear();
    }
    return inputbuffer;
}

// void neko::luainspector::set_print_eval_prettifier(lua_State* L) {
//     if (lua_gettop(L) == 0) return;

//     const int t = lua_type(L, -1);
//     if (!(t == LUA_TFUNCTION || t == LUA_TNIL)) return;

//     lua_pushlightuserdata(L, __neko_lua_inspector_print_func_lightkey());
//     lua_insert(L, -2);
//     lua_settable(L, LUA_REGISTRYINDEX);
// }

// void neko::luainspector::get_print_eval_prettifier(lua_State* L) const {
//     lua_pushlightuserdata(L, __neko_lua_inspector_print_func_lightkey());
//     lua_gettable(L, LUA_REGISTRYINDEX);
// }

// bool neko::luainspector::apply_prettifier(int index) {
//     get_print_eval_prettifier(L);
//     if (lua_type(L, -1) == LUA_TNIL) {
//         lua_pop(L, 1);
//         return false;
//     }

//     assert(lua_type(L, -1) == LUA_TFUNCTION);
//     lua_pushvalue(L, index);
//     if (LUA_OK == lua_pcall(L, 1, 1, 0)) {
//         lua_remove(L, index);
//         lua_insert(L, index);
//         return true;
//     } else {
//         print_line(adjust_error_msg(L, -1), LUACON_LOG_TYPE_ERROR);
//         lua_pop(L, 1);
//         return false;
//     }
//     return true;
// }

int neko::luainspector::command_line_callback_st(ImGuiInputTextCallbackData* data) noexcept {
    command_line_input_callback_UserData* user_data = (command_line_input_callback_UserData*)data->UserData;
    return reinterpret_cast<neko::luainspector*>(user_data->luainspector_ptr)->command_line_input_callback(data);
}

int neko::luainspector::command_line_input_callback(ImGuiInputTextCallbackData* data) {
    command_line_input_callback_UserData* user_data = (command_line_input_callback_UserData*)data->UserData;

    auto paste_buffer = [data](auto begin, auto end, auto buffer_shift) {
        neko::copy(begin, end, data->Buf + buffer_shift, data->Buf + data->BufSize - 1);
        data->BufTextLen = std::min(static_cast<int>(std::distance(begin, end) + buffer_shift), data->BufSize - 1);
        data->Buf[data->BufTextLen] = '\0';
        data->BufDirty = true;
        data->SelectionStart = data->SelectionEnd;
        data->CursorPos = data->BufTextLen;
    };

    const char* command_beg = nullptr;
    command_beg = neko::find_terminating_word(cmd.data(), cmd.data() + cmd.size(), [this](std::string_view sv) { return sv[0] == ' ' ? 1 : 0; });

    if (data->EventKey == ImGuiKey_Tab) {
        std::string complete = this->try_complete(cmd);
        if (!complete.empty()) {
            paste_buffer(complete.data(), complete.data() + complete.size(), command_beg - cmd.data());
            // neko_log_trace("%s", complete.c_str());
        }
    }
    if (data->EventKey == ImGuiKey_UpArrow) {
        cmd2 = this->read_history(-1);
        paste_buffer(cmd2.data(), cmd2.data() + cmd2.size(), command_beg - cmd.data());
        // neko_log_trace("h:%s", cmd2.c_str());
    }
    if (data->EventKey == ImGuiKey_DownArrow) {
        cmd2 = this->read_history(1);
        paste_buffer(cmd2.data(), cmd2.data() + cmd2.size(), command_beg - cmd.data());
        // neko_log_trace("h:%s", cmd2.c_str());
    }

    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        std::string* str = user_data->Str;
        neko_assert(data->Buf == str->c_str());
        str->resize(data->BufTextLen);
        data->Buf = (char*)str->c_str();
    } else if (user_data->ChainCallback) {
        data->UserData = user_data->ChainCallbackUserData;
        return user_data->ChainCallback(data);
    }
    return 0;
}

bool neko::luainspector::command_line_input(const char* label, std::string* str, ImGuiInputTextFlags flags, ImGuiInputTextCallback callback, void* user_data) {
    IM_ASSERT((flags & ImGuiInputTextFlags_CallbackResize) == 0);
    flags |= ImGuiInputTextFlags_CallbackResize;

    command_line_input_callback_UserData cb_user_data;
    cb_user_data.Str = str;
    cb_user_data.ChainCallback = callback;
    cb_user_data.ChainCallbackUserData = user_data;
    cb_user_data.luainspector_ptr = this;
    return ImGui::InputText(label, (char*)str->c_str(), str->capacity() + 1, flags, command_line_callback_st, &cb_user_data);
}

void neko::luainspector::show_autocomplete() noexcept {
    constexpr ImGuiWindowFlags overlay_flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                                               ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;

    if ((m_input_text_id == ImGui::GetActiveID() || m_should_take_focus) && (!m_current_autocomplete_strings.empty())) {

        ImGui::SetNextWindowBgAlpha(0.9f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::SetNextWindowFocus();

        ImVec2 auto_complete_pos = ImGui::GetItemRectMin();

        auto_complete_pos.y = ImGui::GetItemRectMax().y;

        ImVec2 auto_complete_max_size = ImGui::GetItemRectSize();
        auto_complete_max_size.y = -1.f;
        ImGui::SetNextWindowPos(auto_complete_pos);
        ImGui::SetNextWindowSizeConstraints({0.f, 0.f}, auto_complete_max_size);
        if (ImGui::Begin("##terminal:auto_complete", nullptr, overlay_flags)) {
            ImGui::SetActiveID(m_input_text_id, ImGui::GetCurrentWindow());

            auto print_separator = [this]() {
                ImGui::SameLine(0.f, 0.f);
                int pop = try_push_style(ImGuiCol_Text, ImVec4{0.000f, 0.000f, 0.000f, 0.392f});
                ImGui::TextUnformatted(m_autocomlete_separator.data(), m_autocomlete_separator.data() + m_autocomlete_separator.size());
                ImGui::PopStyleColor(pop);
                ImGui::SameLine(0.f, 0.f);
            };

            int max_displayable_sv = 0;
            float separator_length = ImGui::CalcTextSize(m_autocomlete_separator.data(), m_autocomlete_separator.data() + m_autocomlete_separator.size()).x;
            float total_text_length = ImGui::CalcTextSize("...").x;

            std::vector<std::string_view> autocomplete_text;

            if (m_current_autocomplete_strings.empty()) {
            } else {
                autocomplete_text.reserve(m_current_autocomplete_strings.size());
                for (const std::string& str : m_current_autocomplete_strings) {
                    autocomplete_text.emplace_back(str);
                }
            }

            for (const std::string_view& sv : autocomplete_text) {
                float t_len = ImGui::CalcTextSize(sv.data(), sv.data() + sv.size()).x + separator_length;
                if (t_len + total_text_length < auto_complete_max_size.x) {
                    total_text_length += t_len;
                    ++max_displayable_sv;
                } else {
                    break;
                }
            }

            std::string_view last;
            int pop_count = 0;

            if (max_displayable_sv != 0) {
                const std::string_view& first = autocomplete_text[0];
                pop_count += try_push_style(ImGuiCol_Text, ImVec4{1.000f, 1.000f, 1.000f, 1.000f});
                ImGui::TextUnformatted(first.data(), first.data() + first.size());
                pop_count += try_push_style(ImGuiCol_Text, ImVec4{0.500f, 0.450f, 0.450f, 1.000f});
                for (int i = 1; i < max_displayable_sv; ++i) {
                    const std::string_view vs = autocomplete_text[i];
                    print_separator();
                    ImGui::TextUnformatted(vs.data(), vs.data() + vs.size());
                }
                ImGui::PopStyleColor(pop_count);
                if (max_displayable_sv < static_cast<long>(autocomplete_text.size())) last = autocomplete_text[max_displayable_sv];
            }

            pop_count = 0;
            if (max_displayable_sv < static_cast<long>(autocomplete_text.size())) {

                if (max_displayable_sv == 0) {
                    last = autocomplete_text.front();
                    pop_count += try_push_style(ImGuiCol_Text, ImVec4{1.000f, 1.000f, 1.000f, 1.000f});
                    total_text_length -= separator_length;
                } else {
                    pop_count += try_push_style(ImGuiCol_Text, ImVec4{0.500f, 0.450f, 0.450f, 1.000f});
                    print_separator();
                }

                std::vector<char> buf;
                buf.resize(last.size() + 4);
                std::copy(last.begin(), last.end(), buf.begin());
                std::fill(buf.begin() + last.size(), buf.end(), '.');
                auto size = static_cast<unsigned>(last.size() + 3);
                while (size >= 4 && total_text_length + ImGui::CalcTextSize(buf.data(), buf.data() + size).x >= auto_complete_max_size.x) {
                    buf[size - 4] = '.';
                    --size;
                }
                while (size != 0 && total_text_length + ImGui::CalcTextSize(buf.data(), buf.data() + size).x >= auto_complete_max_size.x) {
                    --size;
                }
                ImGui::TextUnformatted(buf.data(), buf.data() + size);
                ImGui::PopStyleColor(pop_count);
            }

            if (ImGui::IsKeyDown(ImGuiKey_Enter)) {
                cmd.clear();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
}

void neko::luainspector::display(bool* textbox_react) noexcept {

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.1f, 0.1f, 0.1f, 1.0f));

    ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetWindowSize().y - 125);
    if (ImGui::BeginChild("##console_log", size)) {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);

        for (auto& a : messageLog) {
            ImVec4 colour;
            switch (a.second) {
                case LUACON_LOG_TYPE_WARNING:
                    colour = {1.0f, 1.0f, 0.0f, 1.0f};
                    break;
                case LUACON_LOG_TYPE_ERROR:
                    colour = {1.0f, 0.0f, 0.0f, 1.0f};
                    break;
                case LUACON_LOG_TYPE_NOTE:
                    colour = {0.13f, 0.44f, 0.61f, 1.0f};
                    break;
                case LUACON_LOG_TYPE_SUCCESS:
                    colour = {0.0f, 1.0f, 0.0f, 1.0f};
                    break;
                case LUACON_LOG_TYPE_MESSAGE:
                    colour = {1.0f, 1.0f, 1.0f, 1.0f};
                    break;
            }
            ImGui::TextColored(colour, "%s", a.first.c_str());
        }
        ImGui::PopTextWrapPos();

        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();

    ImGui::PopStyleColor();

    if (m_should_take_focus) {
        ImGui::SetKeyboardFocusHere();
        m_should_take_focus = false;
    }

    ImGui::PushItemWidth(-1.f);
    if ((this->command_line_input("##Input", &cmd, ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory) || ImGui::IsItemActive()) && textbox_react != nullptr) {
        *textbox_react = true;
    }
    ImGui::PopItemWidth();

    if (m_input_text_id == 0u) {
        m_input_text_id = ImGui::GetItemID();
    }

    auto call_command = [&]() {
        if (!m_history.empty() && m_history.back() != cmd) {
            m_history.push_back(cmd);
            m_history.erase(m_history.begin());
        }

        m_hindex = m_history.size();

        if (L) {
            const int oldtop = lua_gettop(L);
            bool evalok = try_eval(cmd, true) || try_eval(cmd, false);

            if (evalok && LUA_OK == lua_pcall(L, 0, LUA_MULTRET, 0)) {
                if (oldtop != lua_gettop(L)) print_luastack(oldtop + 1, lua_gettop(L), LUACON_LOG_TYPE_MESSAGE);

                lua_settop(L, oldtop);
            } else {
                const std::string err = adjust_error_msg(L, -1);
                if (evalok || !neko::incomplete_chunk_error(err.c_str(), err.length())) {
                    print_line(err, LUACON_LOG_TYPE_ERROR);
                }
                lua_pop(L, 1);
            }
        } else {
            print_line("Lua state pointer is NULL, commands have no effect", LUACON_LOG_TYPE_ERROR);
        }
        cmd.clear();
    };

    if (m_previously_active_id == m_input_text_id && ImGui::GetActiveID() != m_input_text_id) {
        if (ImGui::IsKeyDown(ImGuiKey_Enter)) {
            call_command();
            m_should_take_focus = true;
        } else {
            m_should_take_focus = false;
        }
    }
    m_previously_active_id = ImGui::GetActiveID();

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);

    show_autocomplete();
}

void neko::luainspector::print_line(const std::string& msg, luainspector_logtype type) noexcept { messageLog.emplace_back(msg, type); }

void neko::luainspector::inspect_table(lua_State* L, inspect_table_config& cfg) {
    auto is_multiline = [](const char* str) -> bool {
        while (*str != '\0') {
            if (*str == '\n') {
                return true;
            }
            str++;
        }
        return false;
    };

    lua_pushnil(L);
    while (lua_next(L, -2) != 0) {

        if (lua_isnil(L, -2)) {
            lua_pop(L, 2);
            break;
        }

        bool edited = false;
        auto name = neko_lua_to<const char*>(L, -2);
        if (cfg.search_str != 0 && !strstr(name, cfg.search_str)) {
            lua_pop(L, 1);
            continue;
        }

        int type = lua_type(L, -1);

        if (cfg.is_non_function && type == LUA_TFUNCTION) {
            goto skip;
        }

        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        static ImGuiTreeNodeFlags tree_node_flags = ImGuiTreeNodeFlags_SpanAllColumns;

        if (type == LUA_TSTRING) {

            bool open = ImGui::TreeNodeEx(name, tree_node_flags);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", lua_typename(L, lua_type(L, -1)));
            ImGui::TableNextColumn();
            // ImGui::Text("%p", lua_topointer(L, -1));

            const char* str_mem = neko_lua_to<const char*>(L, -1);
            size_t buffer_size = 256;
            for (; buffer_size < strlen(str_mem);) buffer_size += 128;
            std::string v(neko_lua_to<const char*>(L, -1), buffer_size);
            if (!is_multiline(str_mem) && strlen(str_mem) < 32) {
                ImGui::TextColored(rgba_to_imvec(40, 220, 55, 255), "\"%s\"", str_mem);
            } else {
                ImGui::TextColored(rgba_to_imvec(40, 220, 55, 255), "\"...\"");
            }

            if (open) {

                ImGui::InputTextMultiline("value", const_cast<char*>(v.c_str()), buffer_size);
                if (ImGui::IsKeyDown(ImGuiKey_Enter) && v != neko_lua_to<const char*>(L, -1)) {
                    edited = true;
                    lua_pop(L, 1);                 // # -1 pop value
                    lua_pushstring(L, v.c_str());  // # -1 push new value
                    lua_setfield(L, -3, name);     // -3 table
                }

                ImGui::TreePop();
            }

        } else if (type == LUA_TNUMBER) {

            bool open = ImGui::TreeNodeEx(name, tree_node_flags);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", lua_typename(L, lua_type(L, -1)));
            ImGui::TableNextColumn();
            ImGui::Text("%f", neko_lua_to<double>(L, -1));

            if (open) {
                auto v = neko_lua_to<double>(L, -1);
                // ImGui::Text("lua_v: %f", v);
                ImGui::InputDouble("value", &v);
                if (ImGui::IsKeyDown(ImGuiKey_Enter) && v != neko_lua_to<double>(L, -1)) {
                    edited = true;
                    lua_pop(L, 1);              // # -1 pop value
                    lua_pushnumber(L, v);       // # -1 push new value
                    lua_setfield(L, -3, name);  // -3 table
                }
                ImGui::TreePop();
            }

        } else if (type == LUA_TFUNCTION) {
            ImGui::TreeNodeEx(name, tree_node_flags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_NoTreePushOnOpen);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", lua_typename(L, lua_type(L, -1)));
            ImGui::TableNextColumn();
            ImGui::TextColored(rgba_to_imvec(110, 180, 255, 255), "%p", lua_topointer(L, -1));

        } else if (type == LUA_TTABLE) {
            // ImGui::Text("lua_v: %p", lua_topointer(L, -1));
            // ImGui::Indent();
            // inspect_table(L);
            // ImGui::Unindent();

            bool open = ImGui::TreeNodeEx(name, tree_node_flags);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", lua_typename(L, lua_type(L, -1)));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("--");
            if (open) {
                inspect_table(L, cfg);
                ImGui::TreePop();
            }

        } else if (type == LUA_TUSERDATA) {

            bool open = ImGui::TreeNodeEx(name, tree_node_flags);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", lua_typename(L, lua_type(L, -1)));
            ImGui::TableNextColumn();
            ImGui::TextColored(rgba_to_imvec(75, 230, 250, 255), "%p", lua_topointer(L, -1));

            if (open) {

                ImGui::Text("lua_v: %p", lua_topointer(L, -1));
                if (lua_getmetatable(L, -1)) {
                    lua_pushstring(L, "__name");
                    lua_gettable(L, -2);
                    if (lua_isstring(L, -1)) {
                        const char* name = lua_tostring(L, -1);
                        ImGui::Text("__name: %s", name);
                    } else {
                        ImGui::Text("__name field is not a string!");
                    }
                    // pop __name value and table
                    lua_pop(L, 2);
                } else {
                    ImGui::TextColored(rgba_to_imvec(240, 0, 0, 255), "Unknown Metatable");
                }

                ImGui::TreePop();
            }
        } else if (type == LUA_TBOOLEAN) {
            ImGui::TreeNodeEx(name, tree_node_flags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_NoTreePushOnOpen);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", lua_typename(L, lua_type(L, -1)));
            ImGui::TableNextColumn();
            ImGui::TextColored(rgba_to_imvec(220, 160, 40, 255), "%s", neko_bool_str(lua_toboolean(L, -1)));
        } else {
            ImGui::TreeNodeEx(name, tree_node_flags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_NoTreePushOnOpen);
            ImGui::TableNextColumn();
            ImGui::TextColored(rgba_to_imvec(240, 0, 0, 255), "Unknown");
            ImGui::TableNextColumn();
            ImGui::Text("Unknown");
        }

    skip:
        if (edited) {
        } else {
            lua_pop(L, 1);
        }
    }
}

int neko::luainspector::luainspector_init(lua_State* L) {

    void* model_mem = lua_newuserdata(L, sizeof(neko::luainspector));

    neko::luainspector* inspector = new (model_mem) neko::luainspector();

    inspector->setL(L);
    inspector->m_history.resize(8);

    return 1;
}

int neko::luainspector::luainspector_get(lua_State* L) {
    neko::luainspector* inspector = neko::luainspector::get_from_registry(L);
    lua_pushlightuserdata(L, inspector);
    return 1;
}

int neko::luainspector::luainspector_draw(lua_State* L) {
    neko::luainspector* model = (neko::luainspector*)lua_touserdata(L, 1);

    if (ImGui::Begin("Inspector")) {

        if (ImGui::BeginTabBar("lua_inspector", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Console")) {
                bool textbox_react;
                model->display(&textbox_react);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Registry")) {
                lua_pushglobaltable(L);  // _G
                static char searchText[256] = "";

                static inspect_table_config config;
                config.search_str = searchText;

                ImGui::InputTextWithHint("Search", "Search...", searchText, IM_ARRAYSIZE(searchText));

                ImGui::Checkbox("Non-Function", &config.is_non_function);

                ImGui::Text("Registry contents:");

                ImVec2 size = ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetWindowSize().y - 180);
                if (ImGui::BeginChild("##lua_registry", size)) {
                    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);

                    const float TEXT_BASE_WIDTH = ImGui::CalcTextSize("A").x;

                    static ImGuiTableFlags flags = ImGuiTableFlags_BordersV | ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoBordersInBody;

                    if (ImGui::BeginTable("lua_inspector_reg", 3, flags)) {
                        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide);
                        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, TEXT_BASE_WIDTH * 12.0f);
                        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, TEXT_BASE_WIDTH * 28.0f);
                        ImGui::TableHeadersRow();

                        inspect_table(L, config);

                        ImGui::EndTable();
                    }

                    ImGui::PopTextWrapPos();

                    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 50) ImGui::SetScrollY(ImGui::GetScrollMaxY());
                }
                ImGui::EndChild();

                lua_pop(L, 1);  // pop _G
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Info")) {
                lua_Integer kb = lua_gc(L, LUA_GCCOUNT, 0);
                lua_Integer bytes = lua_gc(L, LUA_GCCOUNTB, 0);

                // if (!arr.empty() && arr.back() != ((f64)bytes)) {
                //     arr.push_back(((f64)bytes));
                //     arr.erase(arr.begin());
                // }

                ImGui::Text("Lua MemoryUsage: %.2lf mb", ((double)kb / 1024.0f));
                ImGui::Text("Lua Remaining: %.2lf mb", ((double)bytes / 1024.0f));

                if (ImGui::Button("GC")) lua_gc(L, LUA_GCCOLLECT, 0);

                // ImGui::PlotLines("Frame Times", arr.data(), arr.size(), 0, NULL, 0, 4000, ImVec2(0, 80.0f));

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    return 0;
}
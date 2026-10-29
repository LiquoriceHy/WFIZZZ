#include "mcp_server.h"
#include <algorithm>
#include <cstring>
#include "../iot/thing_manager.h"
#include <webclient.h>
#include "xiaozhi.h"        // 提供 xz_app 定义

extern "C" {
#include "drv_es8388.h"
}

extern xiaozhi_app_t xz_app;   // 声明全局应用结构体
#define TAG "MCP"
#define BOARD_NAME "psocedge84"
#define DEFAULT_TOOLCALL_STACK_SIZE 6144

McpServer::McpServer()
{
}

McpServer::~McpServer()
{
    for (auto tool : tools_)
    {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools()
{
    /* 为缩短响应时间，将常用工具置于工具列表开头以复用 prompt cache。
     * 备份原工具列表，添加完成后追加回末尾。 */
    auto original_tools = std::move(tools_);
#if 1
    auto speaker = iot::ThingManager::GetInstance().GetThing("Speaker");
    if (speaker)
    {
        /* 设置音量 */
        AddTool("self.audio_speaker.set_volume",
                "Set the volume of the audio speaker,Must not exceed 100.",
                PropertyList(
        {
            Property("volume", kPropertyTypeInteger, 0, 100)
        }),
        [ = ](const PropertyList & properties) -> ReturnValue
        {
            int volume = properties["volume"].value<int>();
            auto json_str = R"({"method":"SetVolume","parameters":{"volume":)" + std::to_string(volume) + "}}";
            auto command = cJSON_Parse(json_str.c_str());
            if (command)
            {
                speaker->Invoke(command);
                cJSON_Delete(command); /* 使用后释放内存 */
            }
            return true;
        });

        /* 获取当前音量 */
        AddTool("self.audio_speaker.get_volume",
                "Get the current volume of the audio speaker.",
                PropertyList(),
                [ = ](const PropertyList &) -> ReturnValue
        {
            const char *json_str = R"({"method":"GetVolume","parameters":{}})";
            cJSON *cmd = cJSON_Parse(json_str); /* 直接使用 const char* */
            speaker->Invoke(cmd);
            cJSON_Delete(cmd);
            return es8388_volume_get();
        });
    }

    /* 屏幕控制工具 */
    auto screen = iot::ThingManager::GetInstance().GetThing("Screen");
    if (screen)
    {
        /* 设置屏幕亮度 */
        AddTool("self.screen.set_brightness",
                "Set screen brightness 0-100.",
                PropertyList(
        {
            Property("brightness", kPropertyTypeInteger, 0, 100)
        }),
        [ = ](const PropertyList & properties) -> ReturnValue
        {
            int brightness = properties["brightness"].value<int>();
            auto json_str = R"({"method":"SetBrightness","parameters":{"brightness":)" + std::to_string(brightness) + "}}";
            auto command = cJSON_Parse(json_str.c_str());
            if (command)
            {
                screen->Invoke(command);
                cJSON_Delete(command);
            }
            return true;
        });
    }

    /* LED 控制工具：单一控制接口 */
    auto led = iot::ThingManager::GetInstance().GetThing("Led");
    if (led)
    {
        AddTool("self.led.set",
                "Control LED. color: red/green/blue to turn on single LED, red_off/green_off/blue_off to turn off single LED, all to turn on all, off to turn off all.",
                PropertyList({Property("color", kPropertyTypeString)}),
                [ = ](const PropertyList & properties) -> ReturnValue
        {
            std::string color = properties["color"].value<std::string>();
            char json_buf[64];
            snprintf(json_buf, sizeof(json_buf), R"({"method":"SetLed","parameters":{"color":"%s"}})", color.c_str());
            auto command = cJSON_Parse(json_buf);
            if (command)
            {
                led->Invoke(command);
                cJSON_Delete(command);
            }
            return true;
        });
    }
#endif
    /* 将原工具列表追加到末尾 */
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

/* ---- RFC 6455 分片发送 ----
 *
 * 不能使用 wsock_write 发送长消息，其 FIN 位规则为
 *     buflen <= 125            设为 FIN（短帧）
 *     buflen <  WSMSG_MAXSIZE  设为 FIN（1420）
 *     buflen >= WSMSG_MAXSIZE  不设 FIN，且自身不续发后续帧
 * （websocket_client.c:1034-1055）
 *
 * 因此任何超过 1420 字节的载荷都会发出一个"仍有后续"的首帧，之后永远
 * 等不到续帧。真机表现（tools/list 的回复 3688 字节）：
 *     [MCP] send tool reply, tools=14, 3688 bytes
 *     received CLOSE from the websocket server
 * 服务端侧对应输出：
 *     发送MCP消息失败: no close frame received or sent
 *
 * 此处绕开 wsock_write，直接按 RFC 6455 拼帧写 TCP：首帧 OPCODE_TEXT
 * 不带 FIN，中间帧 OPCODE_CONTINUATION 不带 FIN，末帧带 FIN。服务端
 * （websockets 14.2）完整支持重组。
 *
 * 帧头长度：载荷小于 126 时为 2 字节，小于 65536 时为 4 字节。客户端
 * 必须掩码，因此再加 4 字节掩码键。分片大小取 1024，远低于 65536 且
 * 为 TCP 留足空间。
 */
#define XZ_WS_FRAG_SZ   1024

static err_t xz_ws_send_fragment(struct altcp_pcb *pcb, const char *data,
                                 rt_size_t len, rt_uint8_t opcode, rt_bool_t fin)
{
    rt_uint8_t hdr[8];
    rt_size_t hdrlen;
    rt_uint8_t *pkt;
    err_t err;
    int i;

    if (len < 126)
    {
        hdr[0] = (rt_uint8_t)(opcode | (fin ? 0x80 : 0x00));
        hdr[1] = (rt_uint8_t)(0x80 | len);        /* MASK=1 */
        hdrlen = 2;
    }
    else
    {
        hdr[0] = (rt_uint8_t)(opcode | (fin ? 0x80 : 0x00));
        hdr[1] = 0x80 | 126;                      /* MASK=1, 16-bit 长度 */
        hdr[2] = (rt_uint8_t)(len >> 8);
        hdr[3] = (rt_uint8_t)(len & 0xFF);
        hdrlen = 4;
    }

    /* 掩码键：客户端发出的帧必须掩码。以 tick 作为种子即可，
     * RFC 仅要求不可预测，本设备为语音终端，没有安全边界需求。 */
    rt_uint8_t mask[4];
    rt_uint32_t seed = (rt_uint32_t)rt_tick_get();
    for (i = 0; i < 4; i++)
    {
        seed = seed * 1103515245u + 12345u;
        mask[i] = (rt_uint8_t)(seed >> 16);
    }
    rt_memcpy(hdr + hdrlen, mask, 4);
    hdrlen += 4;

    pkt = (rt_uint8_t *)rt_malloc(hdrlen + len);
    if (pkt == RT_NULL)
        return ERR_MEM;

    rt_memcpy(pkt, hdr, hdrlen);
    for (i = 0; i < (int)len; i++)
        pkt[hdrlen + i] = (rt_uint8_t)data[i] ^ mask[i & 3];

    /* altcp_write 必须在 lwIP 核心锁内调用。
     *
     * wsock_write 自己用 LOCK_TCPIP_CORE() 包住整段（websocket_client.c:1002），
     * 这里绕开它直接写 PCB，就必须自己加同一把锁，否则会和 tcpip 线程
     * （收包/ACK/重传）同时改 pcb->sndbuf，轻则丢包重传，重则链表损坏
     * 触发 HardFault。 */
    LOCK_TCPIP_CORE();

    /* altcp_write 不改变数据所有权时要 COPY 标志；len 字段是 u16_t，
     * 单帧 1024 + 帧头远小于 65535，安全。 */
    err = altcp_write(pcb, pkt, (u16_t)(hdrlen + len), TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK)
        err = altcp_output(pcb);

    UNLOCK_TCPIP_CORE();

    rt_free(pkt);
    return err;
}

void McpServer::SendText(const std::string &text)
{
    rt_kprintf("[MCP] send tool reply, tools=%d, %d bytes\n",
               (int)tools_.size(), (int)text.length());

    /* 必须先获取 ws_write_mutex 才能写入。
     *
     * 本函数由 DoToolCall 在 websocket 接收线程中同步调用
     * （见 mcp_server.cpp 末尾），而 xiaozhi.cpp 中所有其它发送操作
     * （hello、listen_start、每一帧 Opus 上行）都会先 take 该锁。不获取锁
     * 的后果是 MCP 回复与音频帧在同一 TCP socket 上交错写入，服务端将
     * 拼出帧边界错乱的数据流，表现为"协议错误"或直接断开连接。
     *
     * 使用 RT_WAITING_NO 而非 FOREVER：无法获取锁说明音频正在发送，MCP
     * 回复延迟数十毫秒后重试即可；但此处没有重试机制，因此改为最多等待
     * 200ms（一帧 Opus 上行远小于该量级）。彻底无法获取则放弃本次回复，
     * 服务端会因超时重发 tools/call，这优于将接收线程阻塞而导致整条
     * 对话链路停止响应。 */
    rt_bool_t locked = RT_FALSE;

    if (xz_app.ws.ws_write_mutex)
    {
        if (rt_mutex_take(xz_app.ws.ws_write_mutex,
                          rt_tick_from_millisecond(200)) == RT_EOK)
            locked = RT_TRUE;
    }

    {
        struct altcp_pcb *pcb = xz_app.ws.clnt.pcb;
        rt_size_t total = text.length();
        rt_size_t off = 0;

        if (pcb == RT_NULL)
        {
            rt_kprintf("[MCP] ws pcb gone, drop reply\n");
        }
        else if (total < WSMSG_MAXSIZE)
        {
            /* 短消息走原路径：wsock_write 在 buflen < WSMSG_MAXSIZE 时才设
             * FIN（源码是严格小于），所以边界值 1420 必须走分片分支，
             * 否则照样发出一个没有 FIN 的帧。 */
            wsock_write(&xz_app.ws.clnt, text.c_str(), (u16_t)total, OPCODE_TEXT);
        }
        else
        {
            /* 长消息分片：首帧 TEXT 无 FIN，中间 CONTINUATION 无 FIN，
             * 末帧 CONTINUATION 带 FIN。 */
            const char *p = text.c_str();
            rt_bool_t first = RT_TRUE;

            while (off < total)
            {
                rt_size_t chunk = total - off;
                rt_bool_t last;

                if (chunk > XZ_WS_FRAG_SZ)
                    chunk = XZ_WS_FRAG_SZ;
                last = (off + chunk >= total);

                if (xz_ws_send_fragment(pcb, p + off, chunk,
                                        first ? OPCODE_TEXT : OPCODE_CONTINUATION,
                                        last) != ERR_OK)
                {
                    rt_kprintf("[MCP] frag send failed at offset %d/%d\n",
                               (int)off, (int)total);
                    break;
                }

                first = RT_FALSE;
                off += chunk;
            }

            rt_kprintf("[MCP] sent %d bytes in %d fragments\n",
                       (int)off, (int)((total + XZ_WS_FRAG_SZ - 1) / XZ_WS_FRAG_SZ));
        }
    }

    if (locked)
        rt_mutex_release(xz_app.ws.ws_write_mutex);
}
void McpServer::SendmcpMessage(const std::string &payload)
{
    std::string message = "{\"session_id\":\"" + std::string(reinterpret_cast<const char *>(xz_app.ws.session_id)) +
                          "\",\"type\":\"mcp\",\"payload\":" + payload + "}";
    McpServer::SendText(message);
}

void McpServer::AddTool(McpTool *tool)
{
    /* 防止重复添加同名工具 */
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool * t)
{
    return t->name() == tool->name();
    }) != tools_.end())
    {
        rt_kprintf(TAG, "Tool %s already added", tool->name().c_str());
        delete tool;
        return;
    }

    rt_kprintf("[MCP] Add tool: %s\n", tool->name().c_str());
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string &name, const std::string &description, const PropertyList &properties, std::function<ReturnValue(const PropertyList &)> callback)
{
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::ParseMessage(const std::string &message)
{
    cJSON *json = cJSON_Parse(message.c_str());
    if (json == nullptr)
    {
        rt_kprintf(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON *capabilities)
{

}

void McpServer::ParseMessage(const cJSON *json)
{
    /* 检查 JSONRPC 版本 */
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0)
    {
        rt_kprintf(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }

    /* 检查 method */
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method))
    {
        rt_kprintf(TAG, "Missing method");
        return;
    }

    auto method_str = std::string(method->valuestring);
    rt_kprintf(TAG, "Received method: %s", method_str.c_str());
    if (method_str.find("notifications") == 0)
    {
        return;
    }

    /* 检查 params */
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params))
    {
        rt_kprintf(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id))
    {
        rt_kprintf(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;

    if (method_str == "initialize")
    {
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += "1.0.0";
        message += "\"}}";
        ReplyResult(id_int, message);
    }
    else if (method_str == "tools/list")
    {
        std::string cursor_str = "";
        if (params != nullptr)
        {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor))
            {
                cursor_str = std::string(cursor->valuestring);
            }
        }
        GetToolsList(id_int, cursor_str);
    }
    else if (method_str == "tools/call")
    {
        if (!cJSON_IsObject(params))
        {
            rt_kprintf(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name))
        {
            rt_kprintf(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments))
        {
            rt_kprintf(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        auto stack_size = cJSON_GetObjectItem(params, "stackSize");
        if (stack_size != nullptr && !cJSON_IsNumber(stack_size))
        {
            rt_kprintf(TAG, "tools/call: Invalid stackSize");
            ReplyError(id_int, "Invalid stackSize");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments, stack_size ? stack_size->valueint : DEFAULT_TOOLCALL_STACK_SIZE);
    }
    else
    {
        rt_kprintf(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string &result)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    McpServer::SendmcpMessage(payload);
}
void McpServer::ReplyError(int id, const std::string &message)
{
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    McpServer::SendmcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string &cursor)
{
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";

    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";

    while (it != tools_.end())
    {
        /* 尚未找到起始位置时继续搜索 */
        if (!found_cursor)
        {
            if ((*it)->name() == cursor)
            {
                found_cursor = true;
            }
            else
            {
                ++it;
                continue;
            }
        }

        /* 添加 tool 前检查总大小 */
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size)
        {
            /* 若添加该 tool 会超出大小限制，设置 next_cursor 并退出循环 */
            next_cursor = (*it)->name();
            break;
        }

        json += tool_json;
        ++it;
    }

    if (json.back() == ',')
    {
        json.pop_back();
    }

    if (json.back() == '[' && !tools_.empty())
    {
        /* 未添加任何 tool 时返回错误 */
        rt_kprintf(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty())
    {
        json += "]}";
    }
    else
    {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }

    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string &tool_name, const cJSON *tool_arguments, int stack_size)
{
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(),
                                  [&tool_name](const McpTool * tool)
    {
        return tool->name() == tool_name;
    });

    if (tool_iter == tools_.end())
    {
        rt_kprintf(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();

    for (auto &argument : arguments)
    {
        bool found = false;
        if (cJSON_IsObject(tool_arguments))
        {
            auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
            if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value))
            {
                argument.set_value<bool>(value->valueint == 1);
                found = true;
            }
            else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value))
            {
                int value_int = value->valueint;
                rt_kprintf("value_int: %d\n", value_int);
                if (argument.has_range())
                {
                    value_int = std::clamp(value_int, argument.min_value(), argument.max_value());
                }

                argument.set_value<int>(value_int);
                found = true;
            }
            else if (argument.type() == kPropertyTypeString && cJSON_IsString(value))
            {
                argument.set_value<std::string>(value->valuestring);
                found = true;
            }
        }

        if (!argument.has_default_value() && !found)
        {
            rt_kprintf(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
            ReplyError(id, "Missing valid argument: " + argument.name());
            return;
        }
    }
    ReplyResult(id, (*tool_iter)->Call(arguments));
}

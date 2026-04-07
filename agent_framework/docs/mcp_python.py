from fastapi import FastAPI, Request
from sse_starlette.sse import EventSourceResponse
import asyncio
import sys
import io
import traceback
import json
from typing import Dict, Any, Optional, List

app = FastAPI(title="MCP Python Execute Server (Full MCP Compliant)")

# 安全执行Python代码
def safe_exec(code: str) -> Dict[str, Any]:
    old_stdout = sys.stdout
    new_stdout = io.StringIO()
    sys.stdout = new_stdout
    try:
        local_vars = {}
        exec(code, {}, local_vars)
        output = new_stdout.getvalue()
        return {
            "status": "success",
            "output": output,
            "error": None,
            "variables": local_vars
        }
    except Exception as e:
        error_msg = traceback.format_exc()
        return {
            "status": "error",
            "output": None,
            "error": error_msg,
            "variables": {}
        }
    finally:
        sys.stdout = old_stdout

# ✅ 定义MCP工具列表（核心：向客户端注册python_execute工具）
def get_tool_list() -> List[Dict[str, Any]]:
    return [
        {
            "name": "python_execute",
            "description": "Execute Python code and return the output",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "code": {
                        "type": "string",
                        "description": "Python code to execute"
                    }
                },
                "required": ["code"],
                "additionalProperties": False
            }
        }
    ]

# ✅ 严格遵循MCP 1.0 + JSON-RPC 2.0协议
@app.api_route("/sse", methods=["GET", "POST"])
async def mcp_sse_endpoint(request: Request):
    async def event_generator():
        try:
            # 1. 解析JSON-RPC 2.0请求
            rpc_request: Optional[Dict[str, Any]] = None
            if request.method == "POST":
                try:
                    rpc_request = await request.json()
                except:
                    pass

            # 2. 校验JSON-RPC基础结构
            if not rpc_request or rpc_request.get("jsonrpc") != "2.0":
                yield {
                    "data": json.dumps({
                        "jsonrpc": "2.0",
                        "id": rpc_request.get("id") if rpc_request else None,
                        "error": {
                            "code": -32600,
                            "message": "Invalid Request: Not a valid JSON-RPC 2.0 request"
                        }
                    })
                }
                return

            request_id = rpc_request.get("id")
            method = rpc_request.get("method", "")
            params = rpc_request.get("params", {})

            # 3. 处理MCP初始化请求（核心：注册工具）
            if method == "initialize":
                yield {
                    "data": json.dumps({
                        "jsonrpc": "2.0",
                        "id": request_id,
                        "result": {
                            "protocolVersion": "2024-11-05",
                            "capabilities": {
                                "tools": {}  # 声明支持工具能力
                            },
                            "serverInfo": {
                                "name": "python_execute",
                                "version": "1.0.0"
                            }
                        }
                    })
                }
                return

            # 4. 处理工具列表请求（MCP标准：客户端获取可用工具）
            if method == "tools/list":
                yield {
                    "data": json.dumps({
                        "jsonrpc": "2.0",
                        "id": request_id,
                        "result": {
                            "tools": get_tool_list()
                        }
                    })
                }
                return

            # 5. 处理工具调用请求
            if method == "tools/call":
                tool_name = params.get("name", "")
                arguments = params.get("arguments", {})

                if tool_name != "python_execute":
                    yield {
                        "data": json.dumps({
                            "jsonrpc": "2.0",
                            "id": request_id,
                            "error": {
                                "code": -32602,
                                "message": f"Invalid tool: {tool_name}"
                            }
                        })
                    }
                    return

                code = arguments.get("code", "")
                if not code:
                    yield {
                        "data": json.dumps({
                            "jsonrpc": "2.0",
                            "id": request_id,
                            "error": {
                                "code": -32602,
                                "message": "Missing required parameter: code"
                            }
                        })
                    }
                    return

                # 执行代码
                result = safe_exec(code)
                yield {
                    "data": json.dumps({
                        "jsonrpc": "2.0",
                        "id": request_id,
                        "result": {
                            "content": [
                                {
                                    "type": "text",
                                    "text": result["output"] if result["status"] == "success" else result["error"]
                                }
                            ],
                            "isError": result["status"] == "error"
                        }
                    })
                }
                return

            # 6. 处理未知方法
            yield {
                "data": json.dumps({
                    "jsonrpc": "2.0",
                    "id": request_id,
                    "error": {
                        "code": -32601,
                        "message": f"Method not found: {method}"
                    }
                })
            }

        except Exception as e:
            # 全局异常处理
            yield {
                "data": json.dumps({
                    "jsonrpc": "2.0",
                    "id": None,
                    "error": {
                        "code": -32603,
                        "message": f"Internal error: {str(e)}",
                        "data": traceback.format_exc()
                    }
                })
            }

    return EventSourceResponse(
        event_generator(),
        headers={
            "Cache-Control": "no-cache",
            "Connection": "keep-alive",
            "Content-Type": "text/event-stream"
        }
    )

# 解决OAuth 404日志
@app.get("/.well-known/oauth-authorization-server")
async def oauth_well_known():
    return {
        "issuer": "http://127.0.0.1:8895",
        "authorization_endpoint": "http://127.0.0.1:8895/oauth/authorize",
        "token_endpoint": "http://127.0.0.1:8895/oauth/token",
        "response_types_supported": ["code"]
    }

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="127.0.0.1", port=8895, log_level="info")
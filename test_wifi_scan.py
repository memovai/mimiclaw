import websocket
import json
import time

# WebSocket服务器地址 - 替换为你的设备IP
WS_URL = "ws://192.168.1.19:18789/"

def on_message(ws, message):
    print("收到消息:")
    print(message)
    print("-" * 50)

def on_error(ws, error):
    print("错误:", error)

def on_close(ws, close_status_code, close_msg):
    print("连接关闭")

def on_open(ws):
    print("连接已打开")
    # 发送消息请求扫描WiFi网络
    message = "扫描附近的WiFi网络"
    ws.send(json.dumps({"type": "message", "content": message, "chat_id": "test_chat"}))
    print(f"已发送: {message}")

if __name__ == "__main__":
    print(f"连接到: {WS_URL}")
    print("测试WiFi扫描功能...")
    print("-" * 50)
    
    websocket.enableTrace(True)
    ws = websocket.WebSocketApp(
        WS_URL,
        on_open=on_open,
        on_message=on_message,
        on_error=on_error,
        on_close=on_close
    )
    
    # 运行WebSocket客户端
    ws.run_forever()

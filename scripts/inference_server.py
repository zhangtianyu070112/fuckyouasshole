"""
WebSocket Inference Server — Generic LLM inference service.

Receives raw prompt text over WebSocket, wraps it with the system prompt
in ChatGLM3 chat format, and streams tokens back token-by-token via
llama.cpp.

The client (cockpit.exe) handles all domain-specific prompt construction
(flight phase detection, Chinese text formatting). This server is a pure
stateless inference engine — zero aviation domain logic.

Usage:
    python inference_server.py [--port 8090] [--model path/to/model.gguf]

Protocol (WebSocket, ws://host:8090):
    Client → Server:  raw prompt text (UTF-8 string, one message per request)
    Server → Client:  {"token":"..."}  per token
                      {"done":true}    end of response
                      {"error":"..."}  on failure

Requires:
    pip install websockets llama-cpp-python
"""

import asyncio
import json
import argparse
import os
import sys
import time

import websockets
from llama_cpp import Llama


# =============================================================================
# System prompt — the only domain-specific constant on the server side
# =============================================================================

SYSTEM_PROMPT = (
    "你是B737-800驾驶舱AI副驾驶。"
    "监控飞行参数，用中文提供简洁的操作建议或态势说明。"
    "输出不超过50字。一切正常时报告"正常"，异常时指出具体问题和建议操作。"
    "你提供的建议仅供参考，不可替代SOP和GPWS硬告警。"
)

DEFAULT_MODEL = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "models", "cockpit-advisor-q4km.gguf"
)


# =============================================================================
# WebSocket handler — pure inference, no domain logic
# =============================================================================

async def handle_connection(websocket, llm):
    """Handle a single WebSocket connection from cockpit.exe.

    Receives: raw user prompt text (the C side already built the Chinese
              flight-state description).
    Returns:  token-by-token JSON stream.
    """
    peer = websocket.remote_address
    print(f"[connect] {peer}")
    t0 = time.time()
    msg_count = 0

    try:
        async for message in websocket:
            msg_count += 1
            user_prompt = message.strip()
            if not user_prompt:
                continue

            # Streaming inference — prompt is already formatted by C client
            try:
                stream = llm.create_chat_completion(
                    messages=[
                        {"role": "system", "content": SYSTEM_PROMPT},
                        {"role": "user", "content": user_prompt},
                    ],
                    max_tokens=64,       # Target ≤50 Chinese characters
                    temperature=0.1,
                    top_p=0.9,
                    stream=True,
                )

                token_count = 0
                for chunk in stream:
                    choices = chunk.get("choices", [])
                    if choices:
                        delta = choices[0].get("delta", {})
                        content = delta.get("content", "")
                        if content:
                            token_count += 1
                            await websocket.send(json.dumps(
                                {"token": content},
                                ensure_ascii=False
                            ))

                # End-of-response marker
                await websocket.send(json.dumps({"done": True}))

                if token_count > 0:
                    elapsed = (time.time() - t0) * 1000
                    print(f"[infer] {peer}: {token_count} tokens in "
                          f"{elapsed:.0f}ms ({elapsed/token_count:.1f}ms/tok)")

            except Exception as e:
                print(f"[error] {peer}: inference failed: {e}")
                await websocket.send(json.dumps(
                    {"error": "inference_failed"},
                    ensure_ascii=False
                ))

    except websockets.exceptions.ConnectionClosed:
        pass
    finally:
        elapsed = (time.time() - t0)
        print(f"[disconnect] {peer}: {msg_count} msgs in {elapsed:.0f}s")


# =============================================================================
# Main
# =============================================================================

async def main():
    parser = argparse.ArgumentParser(
        description="B737 Cockpit AI Co-pilot — Generic LLM Inference Server"
    )
    parser.add_argument("--port", type=int, default=8090,
                        help="WebSocket server port (default: 8090)")
    parser.add_argument("--model", type=str, default=DEFAULT_MODEL,
                        help="Path to GGUF model file")
    parser.add_argument("--host", type=str, default="0.0.0.0",
                        help="Bind address (default: 0.0.0.0)")
    parser.add_argument("--n-gpu-layers", type=int, default=-1,
                        help="GPU layers to offload (-1 = all, default)")
    parser.add_argument("--n-ctx", type=int, default=2048,
                        help="Context size (default: 2048)")
    parser.add_argument("--n-threads", type=int, default=8,
                        help="CPU threads (default: 8)")
    parser.add_argument("--max-tokens", type=int, default=64,
                        help="Max output tokens (default: 64)")
    args = parser.parse_args()

    # --- Load model (once at startup, kept warm in GPU VRAM) ---
    print(f"[init] Loading model: {args.model}")
    if not os.path.exists(args.model):
        print(f"[FATAL] Model file not found: {args.model}")
        print(f"        Expected at: {os.path.abspath(args.model)}")
        sys.exit(1)

    t_load = time.time()
    llm = Llama(
        model_path=args.model,
        n_gpu_layers=args.n_gpu_layers,
        n_ctx=args.n_ctx,
        n_threads=args.n_threads,
        verbose=False,
    )
    load_time = time.time() - t_load
    print(f"[init] Model loaded in {load_time:.1f}s "
          f"(n_gpu_layers={args.n_gpu_layers}, n_ctx={args.n_ctx})")

    # --- Start WebSocket server ---
    print(f"[init] WebSocket server starting on ws://{args.host}:{args.port}")
    print(f"[init] Waiting for cockpit.exe connection...")

    async with websockets.serve(
        lambda ws: handle_connection(ws, llm),
        args.host,
        args.port,
        ping_interval=30,
        ping_timeout=10,
        max_size=8192,            # Prompt text is <1KB, 8KB is plenty
    ):
        await asyncio.Future()  # Run forever


if __name__ == "__main__":
    asyncio.run(main())

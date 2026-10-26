"""A stand-in LLM for Conquest tests: an OpenAI-compatible /v1/chat/completions endpoint on localhost.

    python scripts/conquest_mock_llm.py <port> good|garbage <log file>

`good` answers a commander request with a fixed, valid Conquest plan, and an NPC chat request (the
player talking to HQ) with HQ agreeing to make Figari the main effort (or, asked for "mortars on Figari",
calling the mortars there); `garbage` answers with prose
that holds no plan. Every request's report (the user message the game sent) is appended to the log, so
a test can check what the model was told -- including the commander's memory of earlier decisions.
Nothing leaves the machine and no API key is involved.
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = int(sys.argv[1])
MODE = sys.argv[2]
LOG = sys.argv[3]

PLAN = {
    "priorities": [{"objective": "Tyrone", "weight": 3}, {"objective": "Gravette", "weight": 1}],
    "posture": "attack",
    "buy": "Motorised squad",
    "reason": "Tyrone first: it covers the road south from the airfield.",
    "radio": "We hold the airfield and push south to Tyrone. Every squad: Tyrone.",
}


class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        request = {}
        try:
            request = json.loads(body)
            report = "\n".join(m.get("content", "") for m in request.get("messages", []) if m.get("role") == "user")
        except ValueError:
            report = "(unreadable request)"
        with open(LOG, "a", encoding="utf-8") as log:
            log.write("=== request\n" + report + "\n")
        everything = json.dumps(request) if isinstance(request, dict) else ""
        users = [m.get("content", "") for m in request.get("messages", []) if m.get("role") == "user"] if isinstance(request, dict) else []
        last = users[-1].lower() if users else ""
        if MODE != "good":
            content = "I think we should attack something soon."
        elif "SITUATION REPORT" in everything and "mortars on figari" in last:
            # The player asks HQ for fire support: HQ calls the mortars on Figari.
            content = json.dumps({"say": [{"speaker": "HQ", "text": "Copy, mortars on Figari."}],
                                  "orders": [{"to": "all", "order": "barrage", "value": "Figari"}]})
        elif "SITUATION REPORT" in everything:
            # NPC chat: the player talking to HQ. HQ agrees and asks for the main effort.
            content = json.dumps({"say": [{"speaker": "HQ", "text": "Roger, putting everything on Figari."}],
                                  "orders": [{"to": "all", "order": "priority", "value": "Figari"}]})
        else:
            content = json.dumps(PLAN)
        answer = {"choices": [{"index": 0, "message": {"role": "assistant", "content": content}, "finish_reason": "stop"}]}
        data = json.dumps(answer).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()

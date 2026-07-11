"""
Companion relay server for the foldable chess sensing board.

Runs on the laptop at the venue. Receives the crowd-voted winning move
from the ESP32, validates/applies it with python-chess, asks Stockfish
for the engine's reply, and returns both the human move's from/to
squares (for LED flashing) plus the next round's candidate moves
(top N replies for the engine's position) back to the ESP32.

Install:
    pip install python-chess flask

Run:
    python relay_server.py

Requires a local Stockfish binary. Set STOCKFISH_PATH below.
"""

from flask import Flask, request, jsonify
import chess
import chess.engine

STOCKFISH_PATH = "/usr/local/bin/stockfish"  # adjust to your install
NUM_CANDIDATES = 4

app = Flask(__name__)
board = chess.Board()
engine = chess.engine.SimpleEngine.popen_uci(STOCKFISH_PATH)


def get_top_candidates(n=NUM_CANDIDATES):
    """Return the engine's top n candidate moves in UCI form, as strings."""
    info = engine.analyse(board, chess.engine.Limit(depth=12), multipv=n)
    moves = []
    for entry in info:
        pv = entry.get("pv")
        if pv:
            moves.append(pv[0].uci())
    return moves


@app.route("/move", methods=["POST"])
def receive_move():
    data = request.get_json(force=True)
    move_uci = data.get("move", "")

    try:
        move = chess.Move.from_uci(move_uci)
    except ValueError:
        return jsonify({"error": "invalid move format"}), 400

    if move not in board.legal_moves:
        # Fallback: just pick engine's top move if crowd vote was illegal
        result = engine.play(board, chess.engine.Limit(depth=12))
        move = result.move

    from_sq = chess.square_name(move.from_square)
    to_sq = chess.square_name(move.to_square)
    board.push(move)

    # Engine replies immediately (this becomes the "human" side's move
    # to physically place on the board before the next voting round)
    engine_result = engine.play(board, chess.engine.Limit(depth=15))
    board.push(engine_result.move)

    next_candidates = get_top_candidates()

    return jsonify({
        "from": from_sq,
        "to": to_sq,
        "engine_move": engine_result.move.uci(),
        "next_candidates": next_candidates,
        "fen": board.fen(),
    })


@app.route("/reset", methods=["POST"])
def reset_board():
    global board
    board = chess.Board()
    return jsonify({"status": "reset", "fen": board.fen()})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8000)

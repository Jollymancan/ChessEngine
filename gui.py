import subprocess
import tkinter as tk
from tkinter import simpledialog, messagebox
import sys
import os

START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

UNICODE = {
    "P": "♙", "N": "♘", "B": "♗", "R": "♖", "Q": "♕", "K": "♔",
    "p": "♟", "n": "♞", "b": "♝", "r": "♜", "q": "♛", "k": "♚",
}

def fen_to_board(fen_piece_placement: str):
    # returns list of 64 chars or "."
    board = ["." for _ in range(64)]
    ranks = fen_piece_placement.split("/")
    if len(ranks) != 8:
        raise ValueError("Bad FEN placement")
    sq = 56  # a8
    for r in ranks:
        f = 0
        for ch in r:
            if ch.isdigit():
                f += int(ch)
                sq += int(ch)
            else:
                board[sq] = ch
                f += 1
                sq += 1
        sq -= 16
    return board

def parse_start_fen(fen: str):
    parts = fen.split()
    return fen_to_board(parts[0])

def sq_to_alg(sq: int) -> str:
    f = sq % 8
    r = sq // 8
    return chr(ord("a") + f) + chr(ord("1") + r)

def alg_to_sq(a: str) -> int:
    f = ord(a[0]) - ord("a")
    r = ord(a[1]) - ord("1")
    return r * 8 + f

def is_white_piece(p: str) -> bool:
    return p.isalpha() and p.isupper()

def is_black_piece(p: str) -> bool:
    return p.isalpha() and p.islower()

def apply_uci_move(board, uci: str):
    # Minimal board updater (enough for display)
    # Handles: captures, promotions, castling, en passant (heuristic)
    if len(uci) < 4:
        return

    frm = alg_to_sq(uci[0:2])
    to = alg_to_sq(uci[2:4])
    promo = uci[4] if len(uci) >= 5 else None

    piece = board[frm]
    board[frm] = "."

    # Castling: king moves two squares
    if piece in ("K", "k") and abs((to % 8) - (frm % 8)) == 2:
        # move rook accordingly
        if to % 8 == 6:  # king side
            rook_from = (frm // 8) * 8 + 7
            rook_to = (frm // 8) * 8 + 5
        else:  # queen side
            rook_from = (frm // 8) * 8 + 0
            rook_to = (frm // 8) * 8 + 3
        rook = board[rook_from]
        board[rook_from] = "."
        board[rook_to] = rook

    # En passant heuristic: pawn moves diagonally to empty square
    if piece in ("P", "p"):
        if (frm % 8) != (to % 8) and board[to] == ".":
            # captured pawn is behind target square
            cap_sq = to - 8 if piece == "P" else to + 8
            if 0 <= cap_sq < 64:
                board[cap_sq] = "."

    # Promotion
    if promo and piece in ("P", "p"):
        if piece == "P":
            piece = promo.upper()
        else:
            piece = promo.lower()

    # Normal capture is just overwrite (board[to] replaced)
    board[to] = piece

class UCIEngine:
    def __init__(self, exe_path):
        self.p = subprocess.Popen(
            [exe_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1
        )
        self._handshake()

    def send(self, cmd: str):
        if self.p.stdin:
            self.p.stdin.write(cmd + "\n")
            self.p.stdin.flush()

    def read_line(self):
        if not self.p.stdout:
            return ""
        return self.p.stdout.readline()

    def read_until(self, prefix: str, max_lines=10000):
        for _ in range(max_lines):
            line = self.read_line()
            if not line:
                return ""
            line = line.strip()
            if line.startswith(prefix):
                return line
        return ""

    def _handshake(self):
        self.send("uci")
        self.read_until("uciok")
        self.send("isready")
        self.read_until("readyok")

    def new_game(self):
        self.send("ucinewgame")
        self.send("isready")
        self.read_until("readyok")

    def bestmove_from_startpos_moves(self, moves, movetime_ms=200):
        # set position by move list
        if moves:
            self.send("position startpos moves " + " ".join(moves))
        else:
            self.send("position startpos")
        self.send(f"go movetime {movetime_ms}")
        # ignore info lines until bestmove
        while True:
            line = self.read_line()
            if not line:
                return None
            line = line.strip()
            if line.startswith("bestmove"):
                parts = line.split()
                if len(parts) >= 2:
                    bm = parts[1]
                    if bm == "0000":
                        return None
                    return bm
                return None

    def quit(self):
        try:
            self.send("quit")
        except Exception:
            pass
        try:
            self.p.terminate()
        except Exception:
            pass

class ChessGUI:
    def __init__(self, root, engine_exe):
        self.root = root
        self.root.title("Chessy (Tkinter)")

        self.engine = UCIEngine(engine_exe)

        self.square_size = 72
        self.flip = False

        self.board = parse_start_fen(START_FEN)
        self.moves = []  # list of UCI moves from startpos
        self.side_to_move = "w"  # purely for UX; engine is truth
        self.selected_sq = None

        self.canvas = tk.Canvas(root, width=8*self.square_size, height=8*self.square_size)
        self.canvas.grid(row=0, column=0, columnspan=4, padx=8, pady=8)
        self.canvas.bind("<Button-1>", self.on_click)

        tk.Button(root, text="New Game", command=self.new_game).grid(row=1, column=0, sticky="ew", padx=8, pady=4)
        tk.Button(root, text="Engine Move (200ms)", command=lambda: self.engine_move(200)).grid(row=1, column=1, sticky="ew", padx=8, pady=4)
        tk.Button(root, text="Auto Play (200ms)", command=lambda: self.auto_play(200)).grid(row=1, column=2, sticky="ew", padx=8, pady=4)
        tk.Button(root, text="Flip Board", command=self.toggle_flip).grid(row=1, column=3, sticky="ew", padx=8, pady=4)

        self.status = tk.StringVar()
        self.status.set("Click a piece, then click destination. You play White by default.")
        tk.Label(root, textvariable=self.status).grid(row=2, column=0, columnspan=4, sticky="w", padx=8, pady=4)

        self.draw()

        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    def toggle_flip(self):
        self.flip = not self.flip
        self.draw()

    def new_game(self):
        self.engine.new_game()
        self.board = parse_start_fen(START_FEN)
        self.moves = []
        self.side_to_move = "w"
        self.selected_sq = None
        self.status.set("New game.")
        self.draw()

    def engine_move(self, ms):
        bm = self.engine.bestmove_from_startpos_moves(self.moves, ms)
        if bm is None:
            self.status.set("Engine has no legal moves (mate/stalemate).")
            return
        self.moves.append(bm)
        apply_uci_move(self.board, bm)
        self.side_to_move = "b" if self.side_to_move == "w" else "w"
        self.selected_sq = None
        self.status.set(f"Engine played {bm}")
        self.draw()

    def auto_play(self, ms):
        # one engine plays both sides, keeps moving until stop button (close window) or game ends
        def step():
            bm = self.engine.bestmove_from_startpos_moves(self.moves, ms)
            if bm is None:
                self.status.set("Game over (no legal moves).")
                return
            self.moves.append(bm)
            apply_uci_move(self.board, bm)
            self.side_to_move = "b" if self.side_to_move == "w" else "w"
            self.selected_sq = None
            self.status.set(f"Auto: {bm}")
            self.draw()
            # schedule next move
            self.root.after(50, step)
        step()

    def on_click(self, event):
        f = event.x // self.square_size
        r = 7 - (event.y // self.square_size)

        if self.flip:
            f = 7 - f
            r = 7 - r

        if not (0 <= f < 8 and 0 <= r < 8):
            return

        sq = r * 8 + f
        piece = self.board[sq]

        if self.selected_sq is None:
            if piece == ".":
                return
            # basic UX: assume user plays side to move, allow selecting only that side’s piece
            if self.side_to_move == "w" and not is_white_piece(piece):
                return
            if self.side_to_move == "b" and not is_black_piece(piece):
                return
            self.selected_sq = sq
            self.status.set(f"Selected {sq_to_alg(sq)}")
            self.draw()
            return

        # second click = destination
        frm = self.selected_sq
        to = sq
        self.selected_sq = None

        uci = sq_to_alg(frm) + sq_to_alg(to)

        # Promotion prompt if pawn reaches last rank (simple heuristic)
        moving = self.board[frm]
        if moving in ("P", "p"):
            to_rank = to // 8
            if (moving == "P" and to_rank == 7) or (moving == "p" and to_rank == 0):
                ans = simpledialog.askstring("Promotion", "Promote to (q,r,b,n):", initialvalue="q")
                if not ans:
                    self.status.set("Promotion cancelled.")
                    self.draw()
                    return
                ans = ans.lower().strip()
                if ans not in ("q","r","b","n"):
                    ans = "q"
                uci += ans

        # Validate by asking engine (parse_uci_move equivalent on engine side by position+go depth 1 isn’t available)
        # We just send it as part of moves; if illegal, engine may behave oddly, so we do a lightweight local reject:
        # Reject moving from empty / capturing own piece (basic)
        if moving == ".":
            self.status.set("No piece selected.")
            self.draw()
            return
        dst = self.board[to]
        if dst != ".":
            if is_white_piece(moving) and is_white_piece(dst):
                self.status.set("Cannot capture own piece.")
                self.draw()
                return
            if is_black_piece(moving) and is_black_piece(dst):
                self.status.set("Cannot capture own piece.")
                self.draw()
                return

        # Apply locally + append to move list
        self.moves.append(uci)
        apply_uci_move(self.board, uci)
        self.side_to_move = "b" if self.side_to_move == "w" else "w"
        self.status.set(f"You played {uci}")
        self.draw()

        # engine replies (you vs engine)
        self.root.after(10, lambda: self.engine_move(200))

    def draw(self):
        self.canvas.delete("all")
        light = "#f0d9b5"
        dark = "#b58863"

        for r in range(8):
            for f in range(8):
                rr, ff = r, f
                if self.flip:
                    rr = 7 - r
                    ff = 7 - f

                x0 = f * self.square_size
                y0 = (7 - r) * self.square_size
                x1 = x0 + self.square_size
                y1 = y0 + self.square_size

                color = dark if (rr + ff) % 2 == 0 else light
                self.canvas.create_rectangle(x0, y0, x1, y1, fill=color, outline=color)

                sq = rr * 8 + ff
                p = self.board[sq]
                if p != ".":
                    self.canvas.create_text(
                        x0 + self.square_size/2,
                        y0 + self.square_size/2,
                        text=UNICODE.get(p, p),
                        font=("Segoe UI Symbol", int(self.square_size*0.55)),
                    )

        if self.selected_sq is not None:
            sq = self.selected_sq
            r = sq // 8
            f = sq % 8
            if self.flip:
                r = 7 - r
                f = 7 - f
            x0 = f * self.square_size
            y0 = (7 - r) * self.square_size
            x1 = x0 + self.square_size
            y1 = y0 + self.square_size
            self.canvas.create_rectangle(x0, y0, x1, y1, outline="#00aaff", width=4)

    def on_close(self):
        try:
            self.engine.quit()
        finally:
            self.root.destroy()

def main():
    exe = "chessy.exe"
    if len(sys.argv) >= 2:
        exe = sys.argv[1]

    if not os.path.exists(exe):
        messagebox.showerror("Error", f"Engine not found: {exe}\nRun: python gui.py path\\to\\chessy.exe")
        return

    root = tk.Tk()
    app = ChessGUI(root, exe)
    root.mainloop()

if __name__ == "__main__":
    main()

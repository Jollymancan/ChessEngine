import os
import sys
import threading
import queue
import subprocess
import tkinter as tk
from tkinter import messagebox, simpledialog, filedialog
import time
import random

# ---------------------------
# Optional: python-chess
# ---------------------------
HAS_PYCHESS = True
try:
    import chess
    import chess.pgn
except Exception:
    HAS_PYCHESS = False

# ---------------------------
# Defaults
# ---------------------------
ENGINE_EXE_DEFAULT = "chessy.exe"

SYZYGY_PATH_DEFAULT = r"syzygy"
USE_SYZYGY_DEFAULT = True

AUTO_BOOK_FILENAME = "komodo.bin"
OWNBOOK_DEFAULT = True
BOOK_RANDOM_DEFAULT = True
BOOK_MIN_WEIGHT_DEFAULT = 1
BOOK_MAX_PLY_DEFAULT = 20

HASH_MB_DEFAULT = 256
THREADS_DEFAULT = 1
MOVE_TIME_MS_DEFAULT = 200

PARAM_FILE_DEFAULT = ""  # user can pick; if empty, we create tools/tune/base_params.txt

START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

UNICODE = {
    "P": "♙", "N": "♘", "B": "♗", "R": "♖", "Q": "♕", "K": "♔",
    "p": "♟", "n": "♞", "b": "♝", "r": "♜", "q": "♛", "k": "♚",
}

# ---------------------------
# Simple opening set (UCI move sequences)
#  - 10–12 plies each, to reduce startpos bias without needing external files
# ---------------------------
OPENINGS_UCI = [
    "e2e4 e7e5 g1f3 b8c6 f1b5 a7a6 b5a4 g8f6",
    "d2d4 d7d5 c2c4 e7e6 b1c3 g8f6 g1f3",
    "e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4",
    "e2e4 e7e6 d2d4 d7d5 b1c3 f8b4",
    "d2d4 g8f6 c2c4 g7g6 b1c3 f8g7",
    "c2c4 e7e5 b1c3 g8f6 g2g3 d7d5",
    "e2e4 c7c6 d2d4 d7d5 b1c3 d5e4",
    "d2d4 d7d5 g1f3 g8f6 c2c4 e7e6",
    "e2e4 e7e5 f1c4 g8f6 d2d3 b8c6",
    "c2c4 g8f6 b1c3 e7e6 g2g3 d7d5",
]

# ---------------------------
# UCI Engine wrapper (thread-safe)
# ---------------------------

class UCIEngine:
    """
    Thread-safe UCI wrapper with a dedicated reader thread.
    Sends lines into a queue. You can also attach on_info callback.
    """
    def __init__(self, exe_path, on_info=None, on_raw=None):
        self.exe_path = exe_path
        self.on_info = on_info
        self.on_raw = on_raw

        self._send_lock = threading.Lock()
        self._lines = queue.Queue()
        self._alive = True

        self.p = subprocess.Popen(
            [exe_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0
        )

        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()

        self._handshake()

    def _ensure_alive(self):
        rc = self.p.poll()
        if rc is not None:
            raise RuntimeError(f"Engine exited (return code {rc}).")

    def send(self, cmd: str):
        self._ensure_alive()
        if not self.p.stdin:
            raise RuntimeError("Engine stdin not available.")
        with self._send_lock:
            self.p.stdin.write((cmd + "\n").encode("ascii", errors="replace"))
            self.p.stdin.flush()

    def _reader_loop(self):
        try:
            while self._alive and self.p.stdout:
                b = self.p.stdout.readline()
                if not b:
                    break
                line = b.decode("utf-8", errors="replace").strip()

                if self.on_raw:
                    try:
                        self.on_raw(line)
                    except Exception:
                        pass

                if line.startswith("info") and self.on_info:
                    parsed = self._parse_info(line)
                    try:
                        self.on_info(line, parsed)
                    except Exception:
                        pass

                self._lines.put(line)
        except Exception:
            pass

    def read_until(self, prefix: str, max_lines=200000, timeout_s=5.0) -> str:
        for _ in range(max_lines):
            try:
                line = self._lines.get(timeout=timeout_s)
            except queue.Empty:
                return ""
            if line.startswith(prefix):
                return line
        return ""

    def _handshake(self):
        self.send("uci")
        if not self.read_until("uciok"):
            raise RuntimeError("Did not receive 'uciok' from engine.")
        self.send("isready")
        if not self.read_until("readyok"):
            raise RuntimeError("Did not receive 'readyok' from engine.")

    @staticmethod
    def _parse_info(line: str) -> dict:
        parts = line.split()
        d = {}
        i = 0
        while i < len(parts):
            tok = parts[i]
            if tok == "depth" and i + 1 < len(parts):
                try: d["depth"] = int(parts[i + 1])
                except: pass
                i += 2; continue
            if tok == "nodes" and i + 1 < len(parts):
                try: d["nodes"] = int(parts[i + 1])
                except: pass
                i += 2; continue
            if tok == "time" and i + 1 < len(parts):
                try: d["time_ms"] = int(parts[i + 1])
                except: pass
                i += 2; continue
            if tok == "score" and i + 2 < len(parts):
                d["score_type"] = parts[i + 1]
                try: d["score_value"] = int(parts[i + 2])
                except: pass
                i += 3; continue
            if tok == "pv":
                d["pv"] = " ".join(parts[i + 1:])
                break
            i += 1
        return d

    def quit(self):
        self._alive = False
        try:
            self.send("quit")
        except Exception:
            pass
        try:
            self.p.terminate()
        except Exception:
            pass


# ---------------------------
# Param file helpers (SPSA)
# ---------------------------

def parse_param_file(path: str) -> dict:
    params = {}
    if not os.path.exists(path):
        return params
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            if "=" not in s:
                continue
            k, v = s.split("=", 1)
            k = k.strip()
            v = v.strip()
            try:
                params[k] = int(v)
            except Exception:
                # ignore non-int values
                pass
    return params

def write_param_file(path: str, params: dict, header: str = ""):
    lines = []
    if header:
        for hl in header.splitlines():
            lines.append(f"# {hl}".rstrip())
        lines.append("#")
    # stable ordering for diffs
    for k in sorted(params.keys()):
        lines.append(f"{k}={int(params[k])}")
    os.makedirs(os.path.dirname(path), exist_ok=True) if os.path.dirname(path) else None
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

def default_base_params() -> dict:
    # Conservative defaults; unknown keys are ignored by engine.
    return {
        "KingAttackN": 6,
        "KingAttackB": 6,
        "KingAttackR": 4,
        "KingAttackQ": 10,
        "KingAttackAttackerBonus": 10,
        "KingAttackScale": 1,

        "ThreatHangingMinor": 18,
        "ThreatHangingRook": 28,
        "ThreatHangingQueen": 40,
        "ThreatPawnBonus": 6,

        "HistPruneMinDepth": 8,
        "HistPruneLateBase": 12,
        "HistPruneLateMul": 2,
        "HistPruneThreshold": -2000,

        "LMRGoodHistBonus": -1,
        "LMRBadHistBonus": 1,
        "LMRCheckBonus": -1,
    }

def clamp_int(x, lo, hi):
    return lo if x < lo else hi if x > hi else x

# ---------------------------
# GUI
# ---------------------------

class ChessGUI:
    def __init__(self, root, engine_exe):
        self.root = root
        self.root.title("Chessy — Power GUI (Tkinter)")

        if not HAS_PYCHESS:
            messagebox.showwarning(
                "python-chess not installed",
                "For legal move checking + perfect SAN/PGN, install python-chess:\n\n"
                "  pip install python-chess\n\n"
                "GUI will run, but human moves are disabled."
            )

        self.game = None
        self.node = None
        self.board = chess.Board(START_FEN) if HAS_PYCHESS else None

        self.moves_uci = []
        self._ignore_move_select = False
        self.play_as = "w"
        self.flip = False
        self.square_size = 72

        self.movetime_ms = MOVE_TIME_MS_DEFAULT
        self.hash_mb = HASH_MB_DEFAULT
        self.threads = THREADS_DEFAULT

        self.syzygy_path = SYZYGY_PATH_DEFAULT
        self.use_syzygy = USE_SYZYGY_DEFAULT

        self.param_file = PARAM_FILE_DEFAULT

        self.ownbook = OWNBOOK_DEFAULT
        self.book_random = BOOK_RANDOM_DEFAULT
        self.book_min_weight = BOOK_MIN_WEIGHT_DEFAULT
        self.book_max_ply = BOOK_MAX_PLY_DEFAULT
        self.book_file = self._auto_find_book(engine_exe)

        self.latest_info = {}
        self.searching = False
        self._ui_info_queue = queue.Queue()

        # Tuning state
        self.tuning = False
        self._tuning_stop = threading.Event()

        self._build_layout()

        try:
            self.engine = UCIEngine(engine_exe, on_info=self.on_engine_info, on_raw=self.on_engine_raw)
        except Exception as e:
            messagebox.showerror("Engine Error", f"Failed to start/handshake:\n\n{e}")
            raise

        self.apply_engine_options()
        self.new_game()

        self.root.after(50, self._poll_ui_queue)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

    # ---------------------------
    # UI layout
    # ---------------------------

    def _build_layout(self):
        self.left = tk.Frame(self.root)
        self.left.grid(row=0, column=0, padx=8, pady=8, sticky="n")

        self.right = tk.Frame(self.root)
        self.right.grid(row=0, column=1, padx=8, pady=8, sticky="n")

        self.canvas = tk.Canvas(self.left, width=8*self.square_size, height=8*self.square_size)
        self.canvas.grid(row=0, column=0, columnspan=5)
        self.canvas.bind("<Button-1>", self.on_click)

        tk.Button(self.left, text="New", command=self.new_game).grid(row=1, column=0, sticky="ew", padx=3, pady=4)
        tk.Button(self.left, text="Undo", command=self.undo).grid(row=1, column=1, sticky="ew", padx=3, pady=4)
        tk.Button(self.left, text="Redo", command=self.redo).grid(row=1, column=2, sticky="ew", padx=3, pady=4)
        tk.Button(self.left, text="Flip", command=self.toggle_flip).grid(row=1, column=3, sticky="ew", padx=3, pady=4)
        tk.Button(self.left, text="Engine Move", command=self.engine_move_button).grid(row=1, column=4, sticky="ew", padx=3, pady=4)

        self.status = tk.StringVar(value="Ready.")
        tk.Label(self.left, textvariable=self.status).grid(row=2, column=0, columnspan=5, sticky="w", padx=3, pady=3)

        tk.Label(self.right, text="Engine / Game", font=("Segoe UI", 12, "bold")).grid(row=0, column=0, sticky="w")

        sf = tk.LabelFrame(self.right, text="Settings")
        sf.grid(row=1, column=0, sticky="we", pady=(6, 6))

        row = tk.Frame(sf); row.pack(fill="x", pady=2)
        tk.Label(row, text="Move time (ms):").pack(side="left")
        self.movetime_var = tk.StringVar(value=str(self.movetime_ms))
        tk.Entry(row, width=8, textvariable=self.movetime_var).pack(side="left", padx=6)
        tk.Button(row, text="Set", command=self.set_movetime).pack(side="left")

        row = tk.Frame(sf); row.pack(fill="x", pady=2)
        tk.Label(row, text="Threads:").pack(side="left")
        self.threads_var = tk.StringVar(value=str(self.threads))
        tk.Entry(row, width=8, textvariable=self.threads_var).pack(side="left", padx=6)
        tk.Button(row, text="Apply", command=self.apply_engine_options).pack(side="left")

        row = tk.Frame(sf); row.pack(fill="x", pady=2)
        tk.Label(row, text="You play:").pack(side="left")
        self.play_as_var = tk.StringVar(value="White")
        tk.OptionMenu(row, self.play_as_var, "White", "Black", command=self.set_play_as).pack(side="left", padx=6)

        row = tk.Frame(sf); row.pack(fill="x", pady=2)
        tk.Label(row, text="Hash (MB):").pack(side="left")
        self.hash_var = tk.StringVar(value=str(self.hash_mb))
        tk.Entry(row, width=8, textvariable=self.hash_var).pack(side="left", padx=6)
        tk.Button(row, text="Apply", command=self.apply_engine_options).pack(side="left")

        row = tk.Frame(sf); row.pack(fill="x", pady=2)
        self.use_syzygy_var = tk.BooleanVar(value=self.use_syzygy)
        tk.Checkbutton(row, text="Use Syzygy", variable=self.use_syzygy_var, command=self.apply_engine_options).pack(side="left")
        tk.Button(row, text="Set TB Path", command=self.pick_syzygy_path).pack(side="left", padx=6)

        row = tk.Frame(sf); row.pack(fill="x", pady=2)
        self.ownbook_var = tk.BooleanVar(value=self.ownbook)
        tk.Checkbutton(row, text="Use Book", variable=self.ownbook_var, command=self.apply_engine_options).pack(side="left")
        tk.Button(row, text="Pick Book", command=self.pick_book_file).pack(side="left", padx=6)

        row = tk.Frame(sf); row.pack(fill="x", pady=2)
        tk.Label(row, text="BookMaxPly:").pack(side="left")
        self.book_maxply_var = tk.StringVar(value=str(self.book_max_ply))
        tk.Entry(row, width=6, textvariable=self.book_maxply_var).pack(side="left", padx=6)
        tk.Label(row, text="MinWeight:").pack(side="left")
        self.book_minw_var = tk.StringVar(value=str(self.book_min_weight))
        tk.Entry(row, width=6, textvariable=self.book_minw_var).pack(side="left", padx=6)
        self.book_random_var = tk.BooleanVar(value=self.book_random)
        tk.Checkbutton(row, text="Random", variable=self.book_random_var, command=self.apply_engine_options).pack(side="left")

        # ParamFile (tuning)
        row = tk.Frame(sf); row.pack(fill="x", pady=2)
        tk.Label(row, text="ParamFile:").pack(side="left")
        self.paramfile_var = tk.StringVar(value=self.param_file)
        tk.Entry(row, width=20, textvariable=self.paramfile_var).pack(side="left", padx=6)
        tk.Button(row, text="Pick", command=self.pick_param_file).pack(side="left")

        uf = tk.Frame(self.right)
        uf.grid(row=2, column=0, sticky="we", pady=(0, 6))
        tk.Button(uf, text="Copy PGN", command=self.copy_pgn).pack(side="left", padx=3)
        tk.Button(uf, text="Copy FEN", command=self.copy_fen).pack(side="left", padx=3)
        tk.Button(uf, text="Paste PGN", command=self.paste_pgn).pack(side="left", padx=3)
        tk.Button(uf, text="Set FEN", command=self.set_fen_dialog).pack(side="left", padx=3)

        inf = tk.LabelFrame(self.right, text="Live Search Info")
        inf.grid(row=3, column=0, sticky="we", pady=(0, 6))

        self.depth_var = tk.StringVar(value="Depth: -")
        self.score_var = tk.StringVar(value="Score: -")
        self.nodes_var = tk.StringVar(value="Nodes: -")
        self.time_var  = tk.StringVar(value="Time: -")
        self.pv_var    = tk.StringVar(value="PV: -")

        tk.Label(inf, textvariable=self.depth_var).pack(anchor="w")
        tk.Label(inf, textvariable=self.score_var).pack(anchor="w")
        tk.Label(inf, textvariable=self.nodes_var).pack(anchor="w")
        tk.Label(inf, textvariable=self.time_var).pack(anchor="w")
        tk.Label(inf, textvariable=self.pv_var, wraplength=360, justify="left").pack(anchor="w")

        ml = tk.LabelFrame(self.right, text="Moves (SAN) — click to navigate")
        ml.grid(row=4, column=0, sticky="we")

        self.moves_list = tk.Listbox(ml, width=52, height=14)
        self.moves_list.pack(fill="both", expand=True)
        self.moves_list.bind("<<ListboxSelect>>", self.on_select_move)

        # ---------------------------
        # Tuning panel (SPSA)
        # ---------------------------
        tf = tk.LabelFrame(self.right, text="Tuning (SPSA) — one click")
        tf.grid(row=5, column=0, sticky="we", pady=(6, 0))

        row = tk.Frame(tf); row.pack(fill="x", pady=2)
        tk.Label(row, text="Iter ms:").pack(side="left")
        self.tune_iter_ms_var = tk.StringVar(value="200")
        tk.Entry(row, width=6, textvariable=self.tune_iter_ms_var).pack(side="left", padx=6)

        tk.Label(row, text="Iter games:").pack(side="left")
        self.tune_iter_games_var = tk.StringVar(value="80")
        tk.Entry(row, width=6, textvariable=self.tune_iter_games_var).pack(side="left", padx=6)

        row = tk.Frame(tf); row.pack(fill="x", pady=2)
        tk.Label(row, text="Val ms:").pack(side="left")
        self.tune_val_ms_var = tk.StringVar(value="1000")
        tk.Entry(row, width=6, textvariable=self.tune_val_ms_var).pack(side="left", padx=6)

        tk.Label(row, text="Val games:").pack(side="left")
        self.tune_val_games_var = tk.StringVar(value="200")
        tk.Entry(row, width=6, textvariable=self.tune_val_games_var).pack(side="left", padx=6)

        row = tk.Frame(tf); row.pack(fill="x", pady=2)
        tk.Label(row, text="Iters:").pack(side="left")
        self.tune_iters_var = tk.StringVar(value="10")
        tk.Entry(row, width=6, textvariable=self.tune_iters_var).pack(side="left", padx=6)

        self.tune_start_btn = tk.Button(row, text="Start SPSA", command=self.start_spsa)
        self.tune_start_btn.pack(side="left", padx=6)
        self.tune_stop_btn = tk.Button(row, text="Stop", command=self.stop_spsa, state="disabled")
        self.tune_stop_btn.pack(side="left")

    # ---------------------------
    # Engine options
    # ---------------------------

    def _auto_find_book(self, engine_exe):
        try:
            engine_dir = os.path.dirname(os.path.abspath(engine_exe))
        except Exception:
            engine_dir = os.getcwd()
        candidate = os.path.join(engine_dir, AUTO_BOOK_FILENAME)
        return candidate if os.path.exists(candidate) else ""

    def pick_param_file(self):
        p = filedialog.askopenfilename(
            title="Select params file (ParamFile)",
            filetypes=[("Text files", "*.txt"), ("All files", "*.*")]
        )
        if p:
            self.paramfile_var.set(p)
            self.apply_engine_options()

    def apply_engine_options(self):
        try:
            try:
                self.hash_mb = max(1, int(self.hash_var.get().strip()))
            except Exception:
                self.hash_mb = HASH_MB_DEFAULT
                self.hash_var.set(str(self.hash_mb))

            try:
                self.threads = max(1, int(self.threads_var.get().strip()))
            except Exception:
                self.threads = THREADS_DEFAULT
                self.threads_var.set(str(self.threads))

            self.use_syzygy = bool(self.use_syzygy_var.get())
            self.ownbook = bool(self.ownbook_var.get())
            self.book_random = bool(self.book_random_var.get())

            try:
                self.book_max_ply = max(0, int(self.book_maxply_var.get().strip()))
            except Exception:
                self.book_max_ply = BOOK_MAX_PLY_DEFAULT
                self.book_maxply_var.set(str(self.book_max_ply))

            try:
                self.book_min_weight = max(1, int(self.book_minw_var.get().strip()))
            except Exception:
                self.book_min_weight = BOOK_MIN_WEIGHT_DEFAULT
                self.book_minw_var.set(str(self.book_min_weight))

            # ParamFile
            self.param_file = self.paramfile_var.get().strip() if hasattr(self, "paramfile_var") else ""
            if self.param_file and not os.path.isabs(self.param_file):
                engine_dir = os.path.dirname(os.path.abspath(self.engine.exe_path))
                self.param_file = os.path.join(engine_dir, self.param_file)

            syz_path = self.syzygy_path.strip()
            if syz_path and not os.path.isabs(syz_path):
                engine_dir = os.path.dirname(os.path.abspath(self.engine.exe_path))
                syz_path = os.path.join(engine_dir, syz_path)

            self.engine.send(f"setoption name Hash value {self.hash_mb}")
            self.engine.send(f"setoption name Threads value {self.threads}")
            self.engine.send(f"setoption name UseSyzygy value {'true' if self.use_syzygy else 'false'}")
            self.engine.send(f"setoption name SyzygyPath value {syz_path}")

            self.engine.send(f"setoption name OwnBook value {'true' if self.ownbook else 'false'}")
            self.engine.send(f"setoption name BookFile value {self.book_file}")
            self.engine.send(f"setoption name BookRandom value {'true' if self.book_random else 'false'}")
            self.engine.send(f"setoption name BookMinWeight value {self.book_min_weight}")
            self.engine.send(f"setoption name BookMaxPly value {self.book_max_ply}")

            # Tuning
            self.engine.send(f"setoption name ParamFile value {self.param_file}")

            self.engine.send("isready")
            self.engine.read_until("readyok")

            msg = f"Options applied. Hash={self.hash_mb}MB Threads={self.threads}"
            if self.param_file:
                msg += f" ParamFile={os.path.basename(self.param_file)}"
            if self.book_file and os.path.exists(self.book_file):
                msg += f" Book={os.path.basename(self.book_file)}"
            self.status.set(msg)

        except Exception as e:
            messagebox.showerror("Engine Options Error", str(e))

    def pick_syzygy_path(self):
        p = filedialog.askdirectory(title="Select Syzygy tablebase folder")
        if p:
            self.syzygy_path = p
            self.apply_engine_options()

    def pick_book_file(self):
        p = filedialog.askopenfilename(
            title="Select Polyglot book (.bin)",
            filetypes=[("Polyglot book", "*.bin"), ("All files", "*.*")]
        )
        if p:
            self.book_file = p
            self.apply_engine_options()

    def set_movetime(self):
        try:
            v = int(self.movetime_var.get().strip())
            if v < 1:
                raise ValueError()
            self.movetime_ms = v
            self.status.set(f"Move time set to {self.movetime_ms} ms.")
        except Exception:
            messagebox.showerror("Invalid move time", "Enter a positive integer (ms).")
            self.movetime_var.set(str(self.movetime_ms))

    def set_play_as(self, _choice=None):
        self.play_as = "w" if self.play_as_var.get() == "White" else "b"
        self.status.set(f"You play {'White' if self.play_as=='w' else 'Black'}.")
        self.new_game()

    # ---------------------------
    # SPSA tuning (one-click)
    # ---------------------------

    def _engine_dir(self):
        return os.path.dirname(os.path.abspath(self.engine.exe_path))

    def _ensure_base_paramfile(self):
        """
        If ParamFile isn't set, create tools/tune/base_params.txt and set it.
        """
        engine_dir = self._engine_dir()
        base = self.paramfile_var.get().strip()
        if base:
            if not os.path.isabs(base):
                base = os.path.join(engine_dir, base)
            return base

        base = os.path.join(engine_dir, "tools", "tune", "base_params.txt")
        if not os.path.exists(base):
            write_param_file(
                base,
                default_base_params(),
                header="Auto-created base params. You can edit these anytime."
            )
        self.paramfile_var.set(base)
        self.apply_engine_options()
        return base

    def start_spsa(self):
        if not HAS_PYCHESS:
            messagebox.showerror("Tuning requires python-chess", "Install python-chess: pip install python-chess")
            return
        if self.tuning:
            return

        base = self._ensure_base_paramfile()
        if not os.path.exists(base):
            messagebox.showerror("ParamFile missing", f"Could not find/create ParamFile:\n{base}")
            return

        try:
            iter_ms = max(10, int(self.tune_iter_ms_var.get().strip()))
            iter_games = max(4, int(self.tune_iter_games_var.get().strip()))
            val_ms = max(10, int(self.tune_val_ms_var.get().strip()))
            val_games = max(4, int(self.tune_val_games_var.get().strip()))
            iters = max(1, int(self.tune_iters_var.get().strip()))
        except Exception:
            messagebox.showerror("Bad tuning inputs", "Check your iteration/validation settings.")
            return

        self._tuning_stop.clear()
        self.tuning = True
        self.tune_start_btn.config(state="disabled")
        self.tune_stop_btn.config(state="normal")

        t = threading.Thread(
            target=self._spsa_worker,
            args=(base, iter_ms, iter_games, val_ms, val_games, iters),
            daemon=True
        )
        t.start()

    def stop_spsa(self):
        if self.tuning:
            self._tuning_stop.set()
            self._ui_info_queue.put(("status", "Stopping SPSA after current game..."))

    def _spsa_worker(self, base_path, iter_ms, iter_games, val_ms, val_games, iters):
        try:
            engine_dir = self._engine_dir()
            work = os.path.join(engine_dir, "tools", "tune", "work")
            os.makedirs(work, exist_ok=True)

            # Snapshot baseline at start for validation comparisons
            baseline_snapshot = os.path.join(work, "baseline_snapshot.txt")
            if not os.path.exists(baseline_snapshot):
                # first time: copy base -> baseline
                base_params = parse_param_file(base_path)
                if not base_params:
                    base_params = default_base_params()
                write_param_file(baseline_snapshot, base_params, header="Baseline snapshot for validation.")

            # SPSA hyperparameters (conservative)
            a0 = 6.0        # step size (bigger -> faster but riskier)
            c0 = 2.0        # perturbation size
            alpha = 0.602
            gamma = 0.101

            # Parameter bounds (keep sane). Unknown keys are still fine.
            bounds = {
                "KingAttackN": (0, 30),
                "KingAttackB": (0, 30),
                "KingAttackR": (0, 30),
                "KingAttackQ": (0, 50),
                "KingAttackAttackerBonus": (0, 60),
                "KingAttackScale": (0, 10),

                "ThreatHangingMinor": (0, 80),
                "ThreatHangingRook": (0, 120),
                "ThreatHangingQueen": (0, 200),
                "ThreatPawnBonus": (0, 50),

                "HistPruneMinDepth": (0, 20),
                "HistPruneLateBase": (0, 60),
                "HistPruneLateMul": (0, 8),
                "HistPruneThreshold": (-10000, 0),

                "LMRGoodHistBonus": (-5, 5),
                "LMRBadHistBonus": (-5, 5),
                "LMRCheckBonus": (-5, 5),
            }

            # Choose which keys to tune (you can expand later)
            tune_keys = list(bounds.keys())

            # Ensure base exists
            theta = parse_param_file(base_path)
            if not theta:
                theta = default_base_params()
                write_param_file(base_path, theta, header="Created base params (was empty).")

            self._ui_info_queue.put(("status", f"SPSA started. Iter={iter_ms}ms/{iter_games} games, Val={val_ms}ms/{val_games} games, Iters={iters}"))

            for k in range(1, iters + 1):
                if self._tuning_stop.is_set():
                    break

                ak = a0 / (k ** alpha)
                ck = c0 / (k ** gamma)

                # Rademacher perturbations (+1 / -1)
                delta = {key: (1 if random.random() < 0.5 else -1) for key in tune_keys}

                theta_plus = dict(theta)
                theta_minus = dict(theta)

                for key in tune_keys:
                    lo, hi = bounds[key]
                    step = int(round(ck))
                    if step < 1:
                        step = 1
                    theta_plus[key] = clamp_int(theta.get(key, 0) + step * delta[key], lo, hi)
                    theta_minus[key] = clamp_int(theta.get(key, 0) - step * delta[key], lo, hi)

                plus_path = os.path.join(work, "params_plus.txt")
                minus_path = os.path.join(work, "params_minus.txt")
                write_param_file(plus_path, theta_plus, header=f"SPSA plus (iter {k})")
                write_param_file(minus_path, theta_minus, header=f"SPSA minus (iter {k})")

                # Play plus vs minus at iteration time
                self._ui_info_queue.put(("status", f"Iter {k}/{iters}: playing {iter_games} games @ {iter_ms}ms ..."))
                score_plus = self._match_plus_vs_minus(
                    plus_path, minus_path,
                    games=iter_games,
                    movetime_ms=iter_ms,
                )
                # score_plus is in [0, iter_games], higher is better for plus
                avg = (score_plus / iter_games) if iter_games > 0 else 0.5
                self._ui_info_queue.put(("status", f"Iter {k}: PLUS score {score_plus:.1f}/{iter_games} (avg={avg:.3f})"))

                if self._tuning_stop.is_set():
                    break

                # SPSA gradient estimate:
                # We map avg win rate to y in [-1, +1] centered at 0.5
                # y = 2*(avg-0.5)
                y = 2.0 * (avg - 0.5)

                # Update theta: theta = theta + ak * y * delta
                # If plus performed better (y>0), move in direction of delta
                for key in tune_keys:
                    lo, hi = bounds[key]
                    upd = int(round(ak * y * delta[key]))
                    if upd == 0:
                        # allow tiny progress
                        upd = 1 if (ak * y * delta[key]) > 0.2 else -1 if (ak * y * delta[key]) < -0.2 else 0
                    theta[key] = clamp_int(theta.get(key, 0) + upd, lo, hi)

                write_param_file(base_path, theta, header=f"SPSA updated after iter {k} (ak={ak:.3f}, ck={ck:.3f})")

                # Periodic validation vs baseline snapshot at 1000ms
                if k == 1 or (k % 3 == 0) or (k == iters):
                    if self._tuning_stop.is_set():
                        break
                    self._ui_info_queue.put(("status", f"Validation: base vs baseline @ {val_ms}ms, {val_games} games ..."))
                    score_base = self._match_A_vs_B(
                        a_param=base_path,
                        b_param=baseline_snapshot,
                        games=val_games,
                        movetime_ms=val_ms,
                    )
                    # score_base is from A (current base) perspective
                    self._ui_info_queue.put(("status", f"Validation result: BASE {score_base:.1f}/{val_games} vs BASELINE"))

            self._ui_info_queue.put(("status", f"SPSA finished. Base params saved: {base_path}"))

            # Re-apply base ParamFile to main engine so GUI uses it
            self.root.after(0, self.apply_engine_options)

        except Exception as e:
            self._ui_info_queue.put(("status", f"SPSA error: {e}"))
        finally:
            self.tuning = False
            self.root.after(0, lambda: self.tune_start_btn.config(state="normal"))
            self.root.after(0, lambda: self.tune_stop_btn.config(state="disabled"))
            self._tuning_stop.clear()

    def _match_plus_vs_minus(self, plus_path, minus_path, games, movetime_ms):
        """
        Returns plus score in [0..games] where win=1, draw=0.5, loss=0.
        """
        score_plus = 0.0
        for g in range(games):
            if self._tuning_stop.is_set():
                break
            plus_is_white = (g % 2 == 0)
            res = self._play_one_game(
                white_param=(plus_path if plus_is_white else minus_path),
                black_param=(minus_path if plus_is_white else plus_path),
                movetime_ms=movetime_ms,
            )
            # res is (white_score) in {1,0.5,0} relative to white
            # convert to plus score
            if plus_is_white:
                score_plus += res
            else:
                score_plus += (1.0 - res)  # plus is black: if white scored 1, plus scored 0
            # light progress
            if (g + 1) % 10 == 0 or (g + 1) == games:
                self._ui_info_queue.put(("status", f"  iter games: {g+1}/{games} (plus={score_plus:.1f})"))
        return score_plus

    def _match_A_vs_B(self, a_param, b_param, games, movetime_ms):
        """
        Returns A score in [0..games].
        """
        score_a = 0.0
        for g in range(games):
            if self._tuning_stop.is_set():
                break
            a_is_white = (g % 2 == 0)
            res = self._play_one_game(
                white_param=(a_param if a_is_white else b_param),
                black_param=(b_param if a_is_white else a_param),
                movetime_ms=movetime_ms,
            )
            if a_is_white:
                score_a += res
            else:
                score_a += (1.0 - res)
            if (g + 1) % 20 == 0 or (g + 1) == games:
                self._ui_info_queue.put(("status", f"  val games: {g+1}/{games} (A={score_a:.1f})"))
        return score_a

    def _play_one_game(self, white_param, black_param, movetime_ms):
        """
        Plays one game and returns score from White perspective:
          1.0 = white win, 0.5 draw, 0.0 white loss
        """
        board = chess.Board(START_FEN)

        # pick a random opening line
        opening = random.choice(OPENINGS_UCI)
        opening_moves = opening.split()

        # apply opening
        applied = []
        for uci in opening_moves:
            mv = chess.Move.from_uci(uci)
            if mv in board.legal_moves:
                board.push(mv)
                applied.append(uci)
            else:
                break

        move_list = list(applied)

        exe = self.engine.exe_path
        eng_w = UCIEngine(exe)
        eng_b = UCIEngine(exe)

        try:
            # Determinism-ish: disable book & TB for tuning games
            for eng, pfile, name in [(eng_w, white_param, "W"), (eng_b, black_param, "B")]:
                eng.send("setoption name Threads value 1")
                eng.send(f"setoption name Hash value {self.hash_mb}")
                eng.send("setoption name OwnBook value false")
                eng.send("setoption name UseSyzygy value false")
                eng.send("setoption name MultiPV value 1")
                eng.send(f"setoption name ParamFile value {pfile}")
                eng.send("isready"); eng.read_until("readyok")

            # Play until game end
            max_plies = 400  # safety
            while not board.is_game_over(claim_draw=True) and len(board.move_stack) < max_plies:
                if self._tuning_stop.is_set():
                    # abort as draw to avoid bias
                    return 0.5

                stm_white = (board.turn == chess.WHITE)
                eng = eng_w if stm_white else eng_b

                if move_list:
                    eng.send("position startpos moves " + " ".join(move_list))
                else:
                    eng.send("position startpos")

                eng.send(f"go movetime {movetime_ms}")

                bm = None
                # allow a bit more than movetime
                deadline = time.time() + max(2.0, movetime_ms / 1000.0 + 1.5)
                while time.time() < deadline:
                    try:
                        line = eng._lines.get(timeout=0.2)
                    except queue.Empty:
                        continue
                    if line.startswith("bestmove"):
                        parts = line.split()
                        bm = parts[1] if len(parts) >= 2 else "0000"
                        break

                if not bm or bm == "0000":
                    break

                mv = chess.Move.from_uci(bm)
                if mv not in board.legal_moves:
                    # illegal = loss for side to move
                    return 0.0 if stm_white else 1.0

                board.push(mv)
                move_list.append(bm)

            outcome = board.outcome(claim_draw=True)
            if outcome is None or outcome.winner is None:
                return 0.5
            return 1.0 if outcome.winner == chess.WHITE else 0.0

        finally:
            try: eng_w.quit()
            except: pass
            try: eng_b.quit()
            except: pass

    # ---------------------------
    # Game control
    # ---------------------------

    def new_game(self):
        if HAS_PYCHESS:
            self.board = chess.Board(START_FEN)
            self.game = chess.pgn.Game()
            self.game.setup(self.board)
            self.node = self.game
            self.moves_uci = []
            self._refresh_move_list()
            self._reset_info_panel()
            self.draw()

            try:
                self.engine.send("ucinewgame")
                self.engine.send("isready")
                self.engine.read_until("readyok")
            except Exception:
                pass

            if self.play_as == "b":
                self.root.after(50, self.engine_move_button)
        else:
            self.status.set("Install python-chess to enable gameplay safely.")

    def undo(self):
        if not HAS_PYCHESS:
            return
        if not self.node or self.node.parent is None:
            return
        self.node = self.node.parent
        self.board = self.node.board()
        self._rebuild_uci_from_game()
        self._refresh_move_list()
        self.draw()

    def redo(self):
        if not HAS_PYCHESS:
            return
        if self.node and self.node.variations:
            self.node = self.node.variations[0]
            self.board = self.node.board()
            self._rebuild_uci_from_game()
            self._refresh_move_list()
            self.draw()

    def toggle_flip(self):
        self.flip = not self.flip
        self.draw()

    def copy_fen(self):
        if HAS_PYCHESS:
            fen = self.board.fen()
            self.root.clipboard_clear()
            self.root.clipboard_append(fen)
            self.status.set("FEN copied.")
        else:
            self.status.set("python-chess missing; FEN not available.")

    def set_fen_dialog(self):
        if not HAS_PYCHESS:
            return
        fen = simpledialog.askstring("Set FEN", "Paste FEN:", initialvalue=self.board.fen())
        if not fen:
            return
        try:
            b = chess.Board(fen)
        except Exception as e:
            messagebox.showerror("Bad FEN", str(e))
            return

        self.board = b
        self.game = chess.pgn.Game()
        self.game.setup(b)
        self.node = self.game
        self.moves_uci = []
        self._refresh_move_list()
        self.draw()

    def copy_pgn(self):
        if not HAS_PYCHESS:
            return
        exporter = chess.pgn.StringExporter(headers=True, variations=False, comments=False)
        pgn_str = self.game.accept(exporter) if self.game else ""
        self.root.clipboard_clear()
        self.root.clipboard_append(pgn_str)
        self.status.set("PGN copied (perfect SAN).")

    def paste_pgn(self):
        if not HAS_PYCHESS:
            return
        try:
            text = self.root.clipboard_get()
        except Exception:
            messagebox.showerror("Clipboard", "Clipboard is empty or unavailable.")
            return
        try:
            game = chess.pgn.read_game(io_string(text))
        except Exception as e:
            messagebox.showerror("PGN parse error", str(e))
            return
        if game is None:
            messagebox.showerror("PGN parse error", "Could not parse a game from clipboard text.")
            return

        self.game = game
        node = game
        while node.variations:
            node = node.variations[0]
        self.node = node
        self.board = node.board()
        self._rebuild_uci_from_game()
        self._refresh_move_list()
        self.draw()
        self.status.set("PGN loaded from clipboard.")

    # ---------------------------
    # Engine move / search
    # ---------------------------

    def engine_move_button(self):
        if not HAS_PYCHESS:
            return
        if self.searching or self.tuning:
            return
        self._reset_info_panel()
        t = threading.Thread(target=self._engine_move_worker, daemon=True)
        t.start()

    def _engine_move_worker(self):
        try:
            self.searching = True

            if self.moves_uci:
                self.engine.send("position startpos moves " + " ".join(self.moves_uci))
            else:
                self.engine.send("position startpos")

            self.engine.send(f"go movetime {self.movetime_ms}")

            while True:
                line = self.engine._lines.get(timeout=10.0)
                if line.startswith("bestmove"):
                    parts = line.split()
                    bm = parts[1] if len(parts) >= 2 else "0000"
                    break

            if bm == "0000":
                self._ui_info_queue.put(("status", "Engine has no legal moves (mate/stalemate)."))
                return

            move = chess.Move.from_uci(bm)
            if move not in self.board.legal_moves:
                self._ui_info_queue.put(("status", f"Engine returned illegal? {bm} (state mismatch)"))
                return

            san = self.board.san(move)
            self.board.push(move)
            if self.node:
                if self.node.variations:
                    self.node.variations.clear()
                self.node = self.node.add_variation(move)
            self.moves_uci.append(bm)

            self._ui_info_queue.put(("moved", (bm, san)))
        except Exception as e:
            self._ui_info_queue.put(("error", str(e)))
        finally:
            self.searching = False

    def _poll_ui_queue(self):
        try:
            while True:
                kind, payload = self._ui_info_queue.get_nowait()
                if kind == "status":
                    self.status.set(payload)
                elif kind == "error":
                    messagebox.showerror("Engine error", payload)
                    self.status.set("Engine error.")
                elif kind == "moved":
                    bm, san = payload
                    self.status.set(f"Engine: {bm} ({san})")
                    self._refresh_move_list()
                    self.draw()
        except queue.Empty:
            pass
        self.root.after(50, self._poll_ui_queue)

    # ---------------------------
    # Info parsing -> UI
    # ---------------------------

    def _reset_info_panel(self):
        self.depth_var.set("Depth: -")
        self.score_var.set("Score: -")
        self.nodes_var.set("Nodes: -")
        self.time_var.set("Time: -")
        self.pv_var.set("PV: -")

    def on_engine_info(self, raw_line: str, parsed: dict):
        self.latest_info = parsed
        self.root.after(0, self._apply_info_to_labels, parsed)

    def on_engine_raw(self, line: str):
        if line.startswith("info string"):
            msg = line[len("info string"):].strip()
            if msg:
                self._ui_info_queue.put(("status", msg))

    def _apply_info_to_labels(self, parsed: dict):
        if "depth" in parsed:
            self.depth_var.set(f"Depth: {parsed['depth']}")
        if "nodes" in parsed:
            self.nodes_var.set(f"Nodes: {parsed['nodes']}")
        if "time_ms" in parsed:
            self.time_var.set(f"Time: {parsed['time_ms']} ms")
        if "score_type" in parsed and "score_value" in parsed:
            if parsed["score_type"] == "cp":
                self.score_var.set(f"Score: {parsed['score_value']} cp")
            elif parsed["score_type"] == "mate":
                self.score_var.set(f"Score: mate {parsed['score_value']}")
            else:
                self.score_var.set(f"Score: {parsed['score_type']} {parsed['score_value']}")
        if "pv" in parsed:
            self.pv_var.set(f"PV: {parsed['pv']}")

    # ---------------------------
    # Move list navigation
    # ---------------------------

    def _refresh_move_list(self):
        if not HAS_PYCHESS:
            return
        self._ignore_move_select = True
        try:
            self.moves_list.delete(0, "end")
            if self.game is None:
                return

            b = self.game.board()
            idx = 0
            for uci in self.moves_uci:
                try:
                    mv = chess.Move.from_uci(uci)
                except Exception:
                    break
                if mv not in b.legal_moves:
                    break
                san = b.san(mv)
                b.push(mv)
                prefix = f"{(idx//2)+1}. " if idx % 2 == 0 else ""
                self.moves_list.insert("end", prefix + san)
                idx += 1

            if idx > 0:
                self.moves_list.selection_clear(0, "end")
                self.moves_list.selection_set(idx - 1)
                self.moves_list.see(idx - 1)
        finally:
            self._ignore_move_select = False

    def on_select_move(self, _evt):
        if not HAS_PYCHESS:
            return
        if getattr(self, "_ignore_move_select", False):
            return
        sel = self.moves_list.curselection()
        if not sel:
            return
        target_ply = sel[0] + 1

        node = self.game
        ply = 0
        while ply < target_ply and node.variations:
            node = node.variations[0]
            ply += 1

        self.node = node
        self.board = node.board()
        self._rebuild_uci_from_game()
        self._refresh_move_list()
        self.draw()

    def _rebuild_uci_from_game(self):
        self.moves_uci = []
        if not HAS_PYCHESS or self.node is None:
            return
        path = []
        n = self.node
        while n and n.parent is not None:
            path.append(n.move.uci())
            n = n.parent
        path.reverse()
        self.moves_uci = path

    def on_click(self, event):
        if not HAS_PYCHESS:
            self.status.set("Install python-chess to enable legal human moves.")
            return
        if self.searching or self.tuning:
            return

        stm = "w" if self.board.turn == chess.WHITE else "b"
        if stm != self.play_as:
            return

        f = event.x // self.square_size
        r = 7 - (event.y // self.square_size)
        if self.flip:
            f = 7 - f
            r = 7 - r
        if not (0 <= f < 8 and 0 <= r < 8):
            return

        sq = chess.square(f, r)
        piece = self.board.piece_at(sq)

        if getattr(self, "selected_sq", None) is None:
            if not piece:
                return
            if (piece.color == chess.WHITE and self.play_as != "w") or (piece.color == chess.BLACK and self.play_as != "b"):
                return
            self.selected_sq = sq
            self.legal_targets = self._legal_targets_from(sq)
            self.status.set(f"Selected {chess.square_name(sq)}")
            self.draw()
            return

        frm = self.selected_sq
        to = sq
        self.selected_sq = None

        candidates = [m for m in self.board.legal_moves if m.from_square == frm and m.to_square == to]
        if not candidates:
            self.status.set("Illegal move.")
            self.legal_targets = set()
            self.draw()
            return

        move = candidates[0]
        if len(candidates) > 1:
            promo = simpledialog.askstring("Promotion", "Promote to (q,r,b,n):", initialvalue="q")
            if not promo:
                self.status.set("Promotion cancelled.")
                self.legal_targets = set()
                self.draw()
                return
            promo = promo.lower().strip()
            promo_map = {"q": chess.QUEEN, "r": chess.ROOK, "b": chess.BISHOP, "n": chess.KNIGHT}
            pt = promo_map.get(promo, chess.QUEEN)
            for m in candidates:
                if m.promotion == pt:
                    move = m
                    break

        san = self.board.san(move)
        self.board.push(move)
        if self.node:
            if self.node.variations:
                self.node.variations.clear()
            self.node = self.node.add_variation(move)
        self.moves_uci.append(move.uci())
        self.status.set(f"You: {move.uci()} ({san})")
        self.legal_targets = set()
        self._refresh_move_list()
        self.draw()

        self.root.after(50, self.engine_move_button)

    def _legal_targets_from(self, from_sq):
        out = set()
        for m in self.board.legal_moves:
            if m.from_square == from_sq:
                out.add(m.to_square)
        return out

    # ---------------------------
    # Drawing
    # ---------------------------

    def draw(self):
        self.canvas.delete("all")
        light = "#f0d9b5"
        dark = "#b58863"

        selected = getattr(self, "selected_sq", None)
        legal_targets = getattr(self, "legal_targets", set())

        last_move = self.board.peek() if (HAS_PYCHESS and self.board.move_stack) else None
        last_from = last_move.from_square if last_move else None
        last_to = last_move.to_square if last_move else None

        for rank in range(8):
            for file in range(8):
                rr, ff = rank, file
                if self.flip:
                    rr = 7 - rank
                    ff = 7 - file

                x0 = file * self.square_size
                y0 = (7 - rank) * self.square_size
                x1 = x0 + self.square_size
                y1 = y0 + self.square_size

                is_dark = ((rr + ff) % 2 == 0)
                color = dark if is_dark else light

                sq = chess.square(ff, rr)

                if sq == selected:
                    color = "#9cc5ff"
                elif sq in legal_targets:
                    color = "#b8f2b0"
                elif sq == last_from or sq == last_to:
                    color = "#f7e27e"

                self.canvas.create_rectangle(x0, y0, x1, y1, fill=color, outline=color)

                piece = self.board.piece_at(sq)
                if piece:
                    sym = piece.symbol()
                    self.canvas.create_text(
                        x0 + self.square_size/2,
                        y0 + self.square_size/2,
                        text=UNICODE.get(sym, sym),
                        font=("Segoe UI Symbol", int(self.square_size * 0.55))
                    )

    # ---------------------------
    # Close
    # ---------------------------

    def on_close(self):
        try:
            self.engine.quit()
        finally:
            self.root.destroy()


# ---------------------------
# Helpers
# ---------------------------

class io_string:
    """Small helper so we don't need import io; keeps file single."""
    def __init__(self, s): self.s = s
    def readline(self):
        if not self.s:
            return ""
        i = self.s.find("\n")
        if i == -1:
            line, self.s = self.s, ""
            return line
        line = self.s[:i+1]
        self.s = self.s[i+1:]
        return line


def main():
    exe = ENGINE_EXE_DEFAULT
    if len(sys.argv) >= 2:
        exe = sys.argv[1]

    if not os.path.exists(exe):
        messagebox.showerror("Error", f"Engine not found: {exe}\nRun: python gui.py path\\to\\chessy.exe")
        return

    root = tk.Tk()
    ChessGUI(root, exe)
    root.mainloop()

if __name__ == "__main__":
    main()

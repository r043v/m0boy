#!/usr/bin/env python3
"""Petite console série pour l'AKA (115200 bauds) : affiche les lignes [perf]
du runtime m0boy, permet de copier / enregistrer.  Nécessite pyserial :
    pip install pyserial
"""
import queue, threading, time, tkinter as tk
from tkinter import ttk, filedialog, messagebox
try:
    import serial, serial.tools.list_ports as lp
except ImportError:
    serial = None


class Console(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("Console série AKA")
        self.geometry("900x520")
        self.q, self.ser, self.stop = queue.Queue(), None, threading.Event()
        bar = ttk.Frame(self); bar.pack(fill="x", padx=6, pady=6)
        self.port = ttk.Combobox(bar, width=14, state="readonly"); self.port.pack(side="left")
        ttk.Button(bar, text="↻", width=3, command=self.refresh).pack(side="left", padx=2)
        self.baud = ttk.Combobox(bar, width=8, values=[9600, 115200, 921600]); self.baud.set(115200)
        self.baud.pack(side="left", padx=4)
        self.btn = ttk.Button(bar, text="Connecter", command=self.toggle); self.btn.pack(side="left")
        ttk.Button(bar, text="Effacer", command=lambda: self.txt.delete("1.0", "end")).pack(side="left", padx=4)
        ttk.Button(bar, text="Enregistrer…", command=self.save).pack(side="left")
        ttk.Button(bar, text="Copier tout", command=self.copy).pack(side="left", padx=4)
        self.auto = tk.BooleanVar(value=True)
        ttk.Checkbutton(bar, text="Défilement auto", variable=self.auto).pack(side="right")
        self.txt = tk.Text(self, bg="#111", fg="#ddd", insertbackground="#ddd", font=("Consolas", 10))
        self.txt.tag_config("perf", foreground="#7fe07f")
        sb = ttk.Scrollbar(self, command=self.txt.yview); self.txt["yscrollcommand"] = sb.set
        sb.pack(side="right", fill="y"); self.txt.pack(fill="both", expand=True)
        entry = ttk.Frame(self); entry.pack(fill="x", padx=6, pady=4)
        self.cmd = ttk.Entry(entry); self.cmd.pack(side="left", fill="x", expand=True)
        self.cmd.bind("<Return>", lambda e: self.send())
        ttk.Button(entry, text="Envoyer", command=self.send).pack(side="left", padx=4)
        if serial is None:
            messagebox.showerror("pyserial manquant", "Installe-le :  pip install pyserial")
        self.refresh(); self.after(50, self.pump)

    def refresh(self):
        ports = [p.device for p in lp.comports()] if serial else []
        self.port["values"] = ports
        if ports and not self.port.get(): self.port.set(ports[-1])

    def toggle(self):
        if self.ser: return self.close()
        try:
            self.ser = serial.Serial(self.port.get(), int(self.baud.get()), timeout=0.2)
        except Exception as e:
            return messagebox.showerror("Connexion impossible", str(e))
        self.stop.clear(); self.btn["text"] = "Déconnecter"
        threading.Thread(target=self.reader, daemon=True).start()

    def close(self):
        self.stop.set()
        time.sleep(0.3)
        try: self.ser.close()
        except Exception: pass
        self.ser = None; self.btn["text"] = "Connecter"

    def reader(self):
        buf = b""
        while not self.stop.is_set():
            try: buf += self.ser.read(512)
            except Exception as e:
                self.q.put(f"[erreur série] {e}\n"); break
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.q.put(line.decode("utf-8", "replace").rstrip("\r") + "\n")

    def pump(self):
        try:
            while True:
                line = self.q.get_nowait()
                self.txt.insert("end", line, "perf" if line.startswith("[perf]") else "")
                if int(self.txt.index("end-1c").split(".")[0]) > 5000: self.txt.delete("1.0", "1000.0")
                if self.auto.get(): self.txt.see("end")
        except queue.Empty:
            pass
        self.after(50, self.pump)

    def send(self):
        if self.ser:
            self.ser.write((self.cmd.get() + "\n").encode()); self.cmd.delete(0, "end")

    def copy(self):
        self.clipboard_clear(); self.clipboard_append(self.txt.get("1.0", "end"))

    def save(self):
        f = filedialog.asksaveasfilename(defaultextension=".txt", initialfile="aka-serie.txt")
        if f: open(f, "w", encoding="utf-8").write(self.txt.get("1.0", "end"))


if __name__ == "__main__":
    Console().mainloop()

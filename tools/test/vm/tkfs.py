# tkfs.py CLASS HELPER(transient|normal|none|fullscreen) HELPER_MS FULLSCREEN_AFTER_HELPER_MS [TRIGGER]
# An X11 (Tk) app going fullscreen like a Wine/Proton game: the main window maps, a helper window takes the keyboard,
# then the main window sends a _NET_WM_STATE fullscreen request (mapped by then, so Hyprland honours it).
# HELPER fullscreen: a dialog (class CLASSdlg) asks instead. Once the TRIGGER file exists, a dialog with a text field
# takes the keyboard. Prints each step and "key KEYSYM [dialog]"
import os
import sys
import tkinter as tk

cls, mode, helper_ms, fs_ms = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
trigger = sys.argv[5] if len(sys.argv) > 5 else None
root = tk.Tk(className=cls)
root.title("MAIN " + cls)
root.geometry("640x400")
tk.Label(root, text="MAIN", bg="blue", fg="white", font=("Sans", 40)).pack(fill="both", expand=True)
wins = {}


def key(e):
    print("key", e.keysym, *(["dialog"] if "dialog" in wins and str(e.widget.winfo_toplevel()) == str(wins["dialog"]) else []), flush=True)


root.bind_all("<Key>", key)


def helper():
    if mode != "none":
        t = tk.Toplevel(root, class_=(cls + "dlg" if mode == "fullscreen" else cls).capitalize())
        t.title("DIALOG " + cls if mode == "fullscreen" else "")
        t.geometry("240x120")
        if mode != "normal":
            t.transient(root)
        tk.Label(t, text="helper", bg="gray").pack(fill="both", expand=True)
        wins["helper"] = t
    print("helper mapped", flush=True)


def fullscreen():
    (wins["helper"] if mode == "fullscreen" else root).attributes("-fullscreen", True)
    print("asked for fullscreen", flush=True)


def dialog():
    if "dialog" not in wins and os.path.exists(trigger):
        t = tk.Toplevel(root)
        t.title("DIALOG " + cls)
        t.geometry("300x150")
        t.transient(root)
        e = tk.Entry(t)
        e.pack(fill="both", expand=True)
        e.focus_set()
        wins["dialog"] = t
        print("dialog mapped", flush=True)
    root.after(100, dialog)


root.after(helper_ms, helper)
root.after(helper_ms + fs_ms, fullscreen)
if trigger:
    dialog()
root.mainloop()

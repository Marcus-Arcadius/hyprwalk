# tkapp.py: an X11 (Tk via XWayland) app for tools/test/vm with override-redirect menus (menu bar, right-click) and a
# tooltip, which the compositor gets as separate windows at absolute positions. Prints a line per event, flushed.
import tkinter as tk
import sys


def say(*words):
    print(*words, flush=True)


root = tk.Tk(className="hyprwalktk")
root.title("hyprwalktk")
root.geometry("600x400")

bar = tk.Menu(root)
filemenu = tk.Menu(bar, tearoff=0)
for item in ("New", "Open", "Save", "Quit"):
    filemenu.add_command(label=item, command=lambda i=item: say("menu", i))
bar.add_cascade(label="File", menu=filemenu)
root.config(menu=bar)

entry = tk.Entry(root, font=("sans", 14))
entry.pack(fill="x", padx=8, pady=6)
entry.bind("<Return>", lambda e: say("text", entry.get()))

frame = tk.Frame(root)
frame.pack(fill="both", expand=True)
lst = tk.Listbox(frame, font=("sans", 12), height=8)
for i in range(200):
    lst.insert("end", f"item {i}")
lst.pack(side="left", fill="y", padx=8)
canvas = tk.Canvas(frame, bg="#40a0e0", highlightthickness=0)
canvas.pack(side="left", fill="both", expand=True)

popup = tk.Menu(root, tearoff=0)
for item in ("Cut", "Copy", "Paste"):
    popup.add_command(label=item, command=lambda i=item: say("menu", i))

tip = None


def show_tip(e):
    global tip
    if tip:
        return
    tip = tk.Toplevel(root)
    tip.wm_overrideredirect(True)
    tip.geometry(f"+{e.x_root + 12}+{e.y_root + 12}")
    tk.Label(tip, text="a tooltip", bg="#ffffc0", relief="solid", borderwidth=1).pack()
    say("tooltip shown")


def hide_tip(e):
    global tip
    if tip:
        tip.destroy()
        tip = None
        say("tooltip hidden")


def on_scroll(*args):
    say("scroll", f"{lst.yview()[0]:.3f}")


canvas.bind("<Button-1>", lambda e: say("click", e.x, e.y))
canvas.bind("<Button-3>", lambda e: (say("button 3"), popup.tk_popup(e.x_root, e.y_root)))
canvas.bind("<Enter>", show_tip)
canvas.bind("<Leave>", hide_tip)
root.bind_all("<Key>", lambda e: say("key", e.keysym))
lst.bind("<Button-4>", lambda e: say("wheel up"))
lst.bind("<Button-5>", lambda e: say("wheel down"))
lst.bind("<MouseWheel>", lambda e: say("wheel", "up" if e.delta > 0 else "down"))
lst.configure(yscrollcommand=on_scroll)
root.bind("<FocusIn>", lambda e: e.widget is root and say("focus in"))
root.bind("<FocusOut>", lambda e: e.widget is root and say("focus out"))
root.bind("<Configure>", lambda e: e.widget is root and say("size", e.width, e.height))
say("started", sys.version.split()[0], tk.TkVersion)
root.mainloop()

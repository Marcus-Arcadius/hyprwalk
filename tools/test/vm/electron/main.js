// main.js: tools/test/vm's Electron app, as Discord is one: a window with page.html, a system notification when it
// starts, a context menu of its own (Electron has none: an app makes one, a native menu here, as VS Code's), and with
// H3D_DIALOG a message box (a window of its own, a dialog of the main one) two seconds after it starts. It prints
// what happens, a line at a time.
const { app, BrowserWindow, Menu, Notification, dialog } = require("electron");
const path = require("path");

app.whenReady().then(() => {
    const w = new BrowserWindow({ width: 900, height: 600, title: "h3d electron", autoHideMenuBar: true });
    w.loadFile(process.env.H3D_PAGE || path.join(__dirname, "page.html"));
    w.webContents.on("page-title-updated", (e, title) => console.log("title " + title));
    w.webContents.on("context-menu", () => {
        const pick = (item) => () => console.log("menu " + item);
        Menu.buildFromTemplate([
            { label: "Copy", click: pick("Copy") },
            { label: "Paste", click: pick("Paste") },
            { label: "Select all", click: pick("Select all") },
        ]).popup({ window: w, callback: () => console.log("menu closed") });
        console.log("menu shown");
    });
    console.log("ozone " + (app.commandLine.getSwitchValue("ozone-platform") || "default"));
    const n = new Notification({ title: "h3d electron", body: "a notification from Electron" });
    n.on("show", () => console.log("notification shown"));
    n.on("failed", (e, err) => console.log("notification failed " + err));
    n.show();
    if (process.env.H3D_DIALOG)
        setTimeout(() => dialog.showMessageBox(w, { message: "a dialog from Electron", buttons: ["OK"] }).then((r) => console.log("dialog " + r.response)), 2000);
});
app.on("window-all-closed", () => app.quit());

// Adds a "Run Lumo File" command (the play button on .lu files).
const vscode = require('vscode');

function activate(context) {
  context.subscriptions.push(vscode.commands.registerCommand('lumo.run', async () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor) return;
    await editor.document.save();
    const lumo = vscode.workspace.getConfiguration('lumo').get('path') || 'lumo';
    const win = process.platform === 'win32'; // VS Code's terminal there is PowerShell
    const quote = s => win ? "'" + s.replace(/'/g, "''") + "'" : "'" + s.replace(/'/g, "'\\''") + "'";
    let term = vscode.window.terminals.find(t => t.name === 'Lumo');
    if (!term) term = vscode.window.createTerminal('Lumo');
    term.show(true);
    term.sendText(`${win ? '& ' : ''}${quote(lumo)} ${quote(editor.document.fileName)}`);
  }));
}

function deactivate() {}

module.exports = { activate, deactivate };

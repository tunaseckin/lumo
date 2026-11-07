// Adds a "Run Lumo File" command (the play button on .lu files).
const vscode = require('vscode');

function activate(context) {
  context.subscriptions.push(vscode.commands.registerCommand('lumo.run', async () => {
    const editor = vscode.window.activeTextEditor;
    if (!editor) return;
    await editor.document.save();
    const lumo = vscode.workspace.getConfiguration('lumo').get('path') || 'lumo';
    const quote = s => "'" + s.replace(/'/g, "'\\''") + "'";
    let term = vscode.window.terminals.find(t => t.name === 'Lumo');
    if (!term) term = vscode.window.createTerminal('Lumo');
    term.show(true);
    term.sendText(`${quote(lumo)} ${quote(editor.document.fileName)}`);
  }));
}

function deactivate() {}

module.exports = { activate, deactivate };

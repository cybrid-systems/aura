// Aura VS Code client: starts `aura lsp` over stdio (Issue #4911).
const vscode = require("vscode");
const { LanguageClient } = require("vscode-languageclient/node");

let client;

function activate(context) {
  const command = vscode.workspace.getConfiguration("aura").get("path") || "aura";
  const server = { command, args: ["lsp"] };
  client = new LanguageClient(
    "aura",
    "Aura",
    { run: server, debug: server },
    { documentSelector: [{ scheme: "file", language: "aura" }] }
  );
  context.subscriptions.push({ dispose: () => client && client.stop() });
  return client.start();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };

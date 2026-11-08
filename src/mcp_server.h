#pragma once

namespace pc {

// Run the Model Context Protocol (MCP) JSON-RPC 2.0 server over stdin/stdout.
// Bridges all tool calls to the GUI Dashboard via Named Pipe (auto-starting GUI if needed).
int RunMcpServer();

} // namespace pc

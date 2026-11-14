#ifndef MOD_OLLAMA_CHAT_REPORT_H
#define MOD_OLLAMA_CHAT_REPORT_H

// --------------------------------------------------------------------------
// What the module tells you, two ways (OllamaChat.Report.*).
//
// In game, a few short lines: .ollama status -- what it talks to, whether it
// is keeping up, what is going on right now, and what looks wrong -- and
// .ollama status chat|talk|city|party|bots for the counts behind one part. Every
// count goes to a file instead: Report.File in the server's logs folder,
// written every Report.Minutes and whenever .ollama report asks.
//
// World thread only. Nothing here is a worker's to read.
// --------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

// .ollama status: a few lines, and what looks wrong.
std::vector<std::string> OllamaStatus_Overview();

// Every count behind one part, or all of them ("all"): "chat" the requests on
// their way, "talk" what goes into prompts, "city" cities, presence and
// mingling, "party" groups and dungeons, "bots" what bots do for people.
// (mod-ollama-chat_command.cpp)
std::vector<std::string> OllamaStatus_Lines(const std::string& want);

// The whole report written to the file now: where it went and how many
// lines. False: it could not be written (`where` says why).
bool OllamaReport_Write(std::string& where, uint32_t& lines);

// Every world tick: the file written again every Report.Minutes.
void OllamaReport_Update(uint32_t diff);

// Where the file is.
std::string OllamaReport_Path();

#endif // MOD_OLLAMA_CHAT_REPORT_H

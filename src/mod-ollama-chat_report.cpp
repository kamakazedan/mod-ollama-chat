#include "mod-ollama-chat_report.h"
#include "mod-ollama-chat_config.h"

#include "GameTime.h"
#include "Log.h"
#include "Timer.h"

#include <fmt/core.h>

#include <ctime>
#include <fstream>

namespace
{
    uint32_t g_reportMs   = 0;
    bool     g_lastFailed = false;

    std::string Uptime()
    {
        const uint64_t s = uint64_t(GameTime::GetUptime().count());
        if (s < 3600)
            return fmt::format("{}m", s / 60);
        if (s < 86400)
            return fmt::format("{}h {}m", s / 3600, s % 3600 / 60);
        return fmt::format("{}d {}h", s / 86400, s % 86400 / 3600);
    }

    // The chat window's colour codes out (|cffrrggbb, |r): a file has none.
    std::string Plain(const std::string& line)
    {
        std::string out;
        out.reserve(line.size());
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] == '|' && i + 1 < line.size())
            {
                if (line[i + 1] == 'r')
                {
                    ++i;
                    continue;
                }
                if (line[i + 1] == 'c' && i + 9 < line.size())
                {
                    i += 9;
                    continue;
                }
            }
            out += line[i];
        }
        return out;
    }
}

std::string OllamaReport_Path()
{
    const std::string& file = g_OllamaReportFile;
    if (!file.empty() && (file.front() == '/' || file.find(':') != std::string::npos))
        return file;   // a whole path
    return sLog->GetLogsDir() + file;
}

bool OllamaReport_Write(std::string& where, uint32_t& lines)
{
    std::vector<std::string> doc;
    auto section = [&doc](const std::string& title, const std::vector<std::string>& body)
    {
        doc.push_back(std::string());
        doc.push_back("== " + title + " ==");
        doc.insert(doc.end(), body.begin(), body.end());
    };
    doc.push_back(fmt::format("Ollama Chat, the full report: {} (the server up {}).",
                              Acore::Time::TimeToTimestampStr(Seconds(std::time(nullptr))), Uptime()));
    doc.push_back(g_OllamaReportMinutes
                      ? fmt::format("Written every {} min and whenever .ollama report asks; .ollama status in game is "
                                    "the short of it.",
                                    g_OllamaReportMinutes)
                      : std::string("Written whenever .ollama report asks; .ollama status in game is the short of it."));
    section("In brief (.ollama status)", OllamaStatus_Overview());
    section("The requests, since start (.ollama status chat)", OllamaStatus_Lines("chat"));
    section("What goes into the prompts (.ollama status talk)", OllamaStatus_Lines("talk"));
    section("Cities, presence and mingling (.ollama status city)", OllamaStatus_Lines("city"));
    section("Groups and dungeons (.ollama status party)", OllamaStatus_Lines("party"));
    section("What bots do for people (.ollama status bots)", OllamaStatus_Lines("bots"));

    const std::string path = OllamaReport_Path();
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (!file)
    {
        where = "could not open " + path + " to write";
        return false;
    }
    for (const std::string& line : doc)
        file << Plain(line) << '\n';
    file.close();
    if (file.fail())
    {
        where = "could not write all of " + path;
        return false;
    }
    where = path;
    lines = uint32_t(doc.size());
    return true;
}

void OllamaReport_Update(uint32_t diff)
{
    if (!g_OllamaReportMinutes)
    {
        g_reportMs = 0;
        return;
    }
    g_reportMs += diff;
    if (g_reportMs < g_OllamaReportMinutes * 60000u)
        return;
    g_reportMs = 0;
    std::string where;
    uint32_t lines = 0;
    const bool ok = OllamaReport_Write(where, lines);
    // Said once when it starts failing, and once when it works again.
    if (!ok && !g_lastFailed)
        LOG_ERROR("module.ollamachat", "[Ollama Chat] The full report was not written: {}.", where);
    else if (ok && g_lastFailed)
        LOG_INFO("module.ollamachat", "[Ollama Chat] The full report is written again: {}.", where);
    g_lastFailed = !ok;
}

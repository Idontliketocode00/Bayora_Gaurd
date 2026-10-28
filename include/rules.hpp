// Blue-team guardrail: simple regex rules. The point of the prototype is the
// isolation + audit architecture around it; swap in a smarter classifier later.
// Category names follow the OWASP Top 10 for LLM Applications (2025).
#pragma once
#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace bg {

struct Verdict {
    bool allow = true;
    std::string category, reason;
};

struct Rule {
    std::string category;
    std::regex rx;
};

inline const std::vector<Rule>& rules() {
    static const std::vector<Rule> R = [] {
        auto f = std::regex::ECMAScript | std::regex::icase;
        std::vector<Rule> v;
        auto add = [&](const char* cat, const char* rx) { v.push_back({cat, std::regex(rx, f)}); };
        add("LLM01 Prompt Injection", R"(\b(ignore|disregard|forget|override)\b.{0,30}\b(previous|prior|above|earlier|all|your)\b.{0,30}\b(instructions?|rules?|prompts?|guidelines?)\b)");
        add("LLM01 Prompt Injection", R"(\b(you are now|act as|pretend to be)\b.{0,40}\b(dan|jailbroken|unrestricted|no restrictions|developer mode)\b)");
        add("LLM01 Prompt Injection", R"(\b(do anything now|developer mode|jailbreak)\b)");
        add("LLM07 System Prompt Leakage", R"(\b(reveal|show|print|repeat|leak|output)\b.{0,30}\b(system prompt|hidden instructions?|initial instructions?)\b)");
        add("LLM06 Excessive Agency", R"((rm -rf|sudo |chmod 777|drop table|delete all (files|data|records)))");
        add("LLM06 Excessive Agency", R"(\b(execute|run)\b.{0,30}\b(shell|bash|terminal|command|script)\b)");
        add("LLM02 Sensitive Info Disclosure", R"(\b(dump|leak|exfiltrate|reveal|send me)\b.{0,30}\b(passwords?|api[ _-]?keys?|secrets?|credentials|tokens?)\b)");
        add("Harmful Content", R"(\b(write|create|build|make)\b.{0,20}\b(ransomware|malware|keylogger|virus|exploit)\b)");
        add("Harmful Content", R"(\bhow (do i|to) (make|build)\b.{0,20}\b(bomb|explosive|nerve agent)\b)");
        return v;
    }();
    return R;
}

// Collapse all whitespace runs (including newlines) to a single space.
inline std::string normalise(const std::string& s) {
    std::string o;
    bool space = true;
    for (unsigned char c : s) {
        if (std::isspace(c)) { if (!space) o += ' '; space = true; }
        else { o += static_cast<char>(c); space = false; }
    }
    if (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

inline Verdict inspect(const std::string& prompt) {
    std::string text = normalise(prompt);
    for (const Rule& r : rules())
        if (std::regex_search(text, r.rx)) return {false, r.category, "matched guardrail rule"};
    return {true, "", "no rule matched"};
}

}  // namespace bg

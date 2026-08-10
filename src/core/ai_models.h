// ai_models.h — portable catalog of on-device AI models (the small Gemma GGUFs the
// managed llama-server loads). Pure data + lookup; downloading/launching the engine is
// platform work that stays in the shell. Part of the "core" per PORTING.md.
#pragma once

#include <string>

namespace minima {

struct AiModelOption {
    const wchar_t* id;
    const wchar_t* label;
    const wchar_t* file;
    const wchar_t* url;
    long long approxBytes;
};

inline const AiModelOption kAiModels[] = {
    {L"gemma-3-1b", L"Gemma 3 1B — fast, ~806 MB", L"gemma-3-1b-it-Q4_K_M.gguf",
     L"https://huggingface.co/unsloth/gemma-3-1b-it-GGUF/resolve/main/gemma-3-1b-it-Q4_K_M.gguf", 806058272LL},
    {L"gemma-3-4b", L"Gemma 3 4B — smarter, ~2.49 GB", L"gemma-3-4b-it-Q4_K_M.gguf",
     L"https://huggingface.co/unsloth/gemma-3-4b-it-GGUF/resolve/main/gemma-3-4b-it-Q4_K_M.gguf", 2489894016LL},
};

/// The model with the given id, or the first (default) if unknown.
inline const AiModelOption& FindAiModel(const std::wstring& id) {
    for (auto& m : kAiModels)
        if (id == m.id) return m;
    return kAiModels[0];
}

} // namespace minima

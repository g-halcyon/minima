// adblock.h — portable ad/tracker host list + suffix matcher. Pure std C++ (part of
// the "core" per PORTING.md). The URL→host parsing stays in each shell (it uses the
// platform's URL API); this matcher is shared everywhere. For full filter-list blocking,
// install an extension like uBlock Origin via Settings > Extensions (Windows only).
#pragma once

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>

namespace minima {

inline const wchar_t* const kAdHosts[] = {
    L"doubleclick.net", L"googlesyndication.com", L"googleadservices.com", L"google-analytics.com",
    L"googletagmanager.com", L"googletagservices.com", L"adservice.google.com", L"adnxs.com",
    L"adsafeprotected.com", L"adsrvr.org", L"amazon-adsystem.com", L"criteo.com", L"criteo.net",
    L"outbrain.com", L"taboola.com", L"scorecardresearch.com", L"quantserve.com", L"quantcount.com",
    L"rubiconproject.com", L"pubmatic.com", L"openx.net", L"casalemedia.com", L"moatads.com",
    L"doubleverify.com", L"2mdn.net", L"adform.net", L"smartadserver.com", L"teads.tv",
    L"yieldmo.com", L"sharethrough.com", L"media.net", L"mgid.com", L"revcontent.com", L"zedo.com",
    L"adcolony.com", L"bidswitch.net", L"33across.com", L"gumgum.com", L"indexww.com", L"sonobi.com",
    L"spotxchange.com", L"triplelift.com", L"undertone.com", L"yieldlab.net", L"hotjar.com",
    L"mouseflow.com", L"fullstory.com", L"crazyegg.com", L"mixpanel.com", L"segment.io",
    L"segment.com", L"amplitude.com", L"branch.io", L"chartbeat.com", L"clarity.ms",
    L"onesignal.com", L"exosrv.com", L"exoclick.com", L"popads.net", L"propellerads.com",
    L"juicyads.com", L"trafficjunky.com",
};

/// True if `host` equals or is a subdomain of a known ad/tracker domain. Case-insensitive.
inline bool IsAdHost(std::wstring host) {
    std::transform(host.begin(), host.end(), host.begin(), towlower);
    for (const wchar_t* d : kAdHosts) {
        size_t dl = wcslen(d);
        if (host.size() == dl && host == d) return true;
        if (host.size() > dl && host.compare(host.size() - dl, dl, d) == 0 &&
            host[host.size() - dl - 1] == L'.')
            return true;
    }
    return false;
}

} // namespace minima

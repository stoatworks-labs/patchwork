/*
 * Stoatworks Labs - About window data for Patchwork.
 *
 * PROVISIONAL HAND COPY, adapted from pitch's generated header on 2026-10-06.
 * stoatworks-backend/scripts/sync-about.py generates this file from the
 * website's projects.json once the plugin is registered there; until then the
 * guide is empty (no user guide exists yet), so no "User guide" button is
 * declared. Landing the plugin in the fleet replaces this file wholesale.
 *
 * `version` here is a fallback read from this repo's own manifest at sync
 * time. Anything with a build step injects the real one at build time and
 * overrides this.
 */
#pragma once

namespace stoatworks::about
{
    inline constexpr auto name = "Patchwork";
    inline constexpr auto slug = "patchwork";
    inline constexpr auto hook = "A faulty LED wall, for Resolume";
    inline constexpr auto licence = "MIT";
    inline constexpr auto guide = "";
    inline constexpr auto page = "https://stoatworks-labs.com/software/patchwork/";
    inline constexpr auto repo = "https://github.com/stoatworks-labs/patchwork";
    inline constexpr auto versionFallback = "v0.1.0";

    inline constexpr auto org = "Stoatworks Labs";
    inline constexpr auto home = "https://stoatworks-labs.com";
    inline constexpr auto tagline = "Open tools for the people who run the show.";

    /* The canonical funding links, matching FUNDING.yml and the support footer. */
    struct Link { const char* name; const char* url; };
    inline constexpr Link funding[] = {
        { "GitHub Sponsors", "https://github.com/sponsors/stoatworks-labs" },
        { "Ko-fi", "https://ko-fi.com/stoatworkslabs" },
        { "Patreon", "https://patreon.com/StoatworksLabs" },
        { "Liberapay", "https://liberapay.com/stoatworks-labs" },
    };
}

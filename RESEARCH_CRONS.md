# Research Cron Jobs

Automated research pipeline — exported 2026-06-04

| Job ID | Name | Schedule | Query/Scope |
|--------|------|----------|-------------|
| `3080d5e7298f` | switch2-smp-research | 0 3 * * * | BTstack SMP encryption, Switch 2 Pro Controller BLE |
| `0e1d19befa62` | Research-Update-Check | 0 10 * * 0 | Crackberry components: Docker, Cloudflare, SearXNG, Kodi, Tailscale |
| `20955a3da848` | arXiv Paper Wochenrückblick | 0 11 * * 0 | AI/ML latest papers via arXiv + Perplexity deep read |
| `2ab18e7f2c68` | OSSInsight AI Trending | 0 10 * * 1 | ossinsight.io/trending/ai new repos |
| `b67fe118d445` | WP-Security Search | 0 10 * * 0 | WordPress security patches 2026 |
| `be5869206ee7` | Daily News Digest | 30 7 * * * | German tech news from RSS feeds |

## switch2-smp-research (`3080d5e7298f`)

Most active research cron. 6 SearXNG queries nightly + Perplexity deep research fallback.
Output saved to `/home/richal/switch2-raw/SMP_RESEARCH.md`.
Skills: `enterprise-research`

### Queries
1. BTstack SMP `SM_AUTHREQ_NO_BONDING` encryption complete `HCI_EVENT_ENCRYPTION_CHANGE`
2. BLE "Just Works" pairing encryption "Write Not Permitted" 0x03 GATT
3. Nintendo Switch Pro Controller BLE GATT pairing encryption required
4. `site:github.com/bluekitchen/btstack` SMP encryption `sm_request_pairing`
5. LE Secure Connections "Encryption Change" event not firing HCI
6. "CareyScott" "649d4ac9" switch 2 pro controller bluetooth pairing command

## Research-Update-Check (`0e1d19befa62`)

Weekly (Sun 10:00). Web research on critical Crackberry components for updates, security issues.
Skills: `enterprise-research`, `source-driven-development`, `spike`

## arXiv Paper Wochenrückblick (`20955a3da848`)

Weekly (Sun 11:00). Monitors arxiv.org for new ML/AI papers. Perplexity deep-reads top papers.
Skills: `arxiv-monitor`, `perplexity-research`

## OSSInsight AI Trending (`2ab18e7f2c68`)

Weekly (Mon 10:00). Watches ossinsight.io/trending/ai for new trending repos.
Script: `ossinsight_watcher.sh`

## WP-Security Search (`b67fe118d445`)

Weekly (Sun 10:00). Web search for latest WordPress security patches.
Skills: `security-and-hardening`

## Daily News Digest (`be5869206ee7`)

Daily (07:30). German tech news from RSS feeds (Heise, Golem, etc.).
Skills: `news-monitor`, `anti-ai-slop-writing`

---

**Research method effectiveness (from switch2-smp-research run):**
- Perplexity/OpenRouter: ★★★★★ — detailed technical analysis
- web_search: ★☆☆☆☆ — surface-level results
- SearXNG: ★☆☆☆☆ — instance down (nginx 404 on /search)
- GitHub Raw Source: ★★★★☆ — direct API/config access

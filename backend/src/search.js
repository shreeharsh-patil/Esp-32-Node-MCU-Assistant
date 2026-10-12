const SEARCH_URL = 'https://api.firecrawl.dev/v2/search';
const MAX_QUERY = 240;
const SEARCH_BUDGET_MS = 10000;
let cursor = 0;

export function firecrawlKeys(env = process.env) {
  const keys = [...new Set((env.FIRECRAWL_API_KEYS || env.FIRECRAWL_API_KEY || '')
    .split(',').map(key => key.trim()).filter(Boolean))];
  if (keys.length > 16 || keys.some(key => key.length > 256 || /\s/.test(key))) {
    throw new Error('Use at most 16 comma-separated Firecrawl keys without whitespace');
  }
  return keys;
}

export async function searchWeb(query, keys, options = {}) {
  if (typeof query !== 'string' || !query.trim() || query.length > MAX_QUERY) {
    return { error: 'Search query must be 1–240 characters.' };
  }
  if (!keys?.length) return { error: 'Live search is not configured.' };
  const fetcher = options.fetch || fetch;
  const deadline = Date.now() + (options.budgetMs || SEARCH_BUDGET_MS);
  const start = cursor++ % keys.length;
  for (let attempt = 0; attempt < keys.length; attempt++) {
    const remaining = deadline - Date.now();
    if (remaining < 500) break;
    const key = keys[(start + attempt) % keys.length];
    try {
      const response = await fetcher(SEARCH_URL, {
        method: 'POST',
        headers: { Authorization: `Bearer ${key}`, 'Content-Type': 'application/json' },
        body: JSON.stringify({ query: query.trim(), sources: ['web'], limit: 3, safe: true }),
        signal: AbortSignal.timeout(Math.min(remaining, 6000)),
      });
      if ([401, 403, 429, 500, 502, 503, 504].includes(response.status)) continue;
      if (!response.ok) return { error: 'Live search could not complete.' };
      const payload = await response.json();
      if (payload.success === false) return { error: 'Live search could not complete.' };
      const found = Array.isArray(payload.data?.web) ? payload.data.web :
        Array.isArray(payload.data) ? payload.data : [];
      const results = found.slice(0, 3).filter(item => {
        try { return new URL(item.url).protocol === 'https:'; } catch { return false; }
      }).map(item => ({
        title: String(item.title || '').slice(0, 150),
        description: String(item.description || '').slice(0, 400),
        url: item.url,
      }));
      return { searchedAt: new Date().toISOString(), results };
    } catch { /* Retry another key within the shared time budget. */ }
  }
  return { error: 'Live search is temporarily unavailable. Please try again.' };
}

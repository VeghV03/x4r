"""HTTP fetching for the converters. Tests replace `fetch` to stay offline."""

import httpx

USER_AGENT = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) x4-companion"
TIMEOUT = 30.0


def fetch(url: str) -> bytes:
    with httpx.Client(follow_redirects=True, timeout=TIMEOUT, headers={"User-Agent": USER_AGENT}) as client:
        response = client.get(url)
        response.raise_for_status()
        return response.content

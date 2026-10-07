package com.anland.consumer;

/**
 * Keeps the last answer from Gold's {@code input-status}, so a key press never
 * has to wait for one.
 *
 * <p>Asking Gold costs an {@code su} round trip. That is fine in the background
 * and unacceptable in a key press: the toggle has to feel like a key, and the
 * only thing the answer changes is which nodes this session declines to try.
 * Trying and failing is already safe — the root helper's grab fails on a node
 * someone else owns and it moves on — so acting on a slightly old answer is
 * strictly better than acting on none, and acting on none beats making the user
 * wait.
 *
 * <p>Android-free: the caller supplies the clock and runs the query, so the
 * freshness rule can be unit tested on the JVM.
 */
final class GoldStatusCache {
    /**
     * How long a verified or unknown answer stays usable. An ABSENT answer is
     * definitive for this purpose: until a later refresh proves otherwise, Gold
     * cannot be holding any nodes if its module is not installed.
     */
    static final long MAX_AGE_MS = 5000L;

    private final GoldInputStatusClient client;

    private GoldInputStatusClient.Result cached;
    private long cachedAt;
    private boolean refreshing;

    GoldStatusCache(GoldInputStatusClient client) {
        this.client = client;
    }

    /**
     * The last answer, or null when there is none or it has aged out.
     *
     * <p>Never blocks, and never queries: the whole point is that the caller is
     * on a key press.
     */
    GoldInputStatusClient.Result fresh(long nowMs) {
        if (cached == null)
            return null;
        if (cached.state == GoldInputStatusClient.State.ABSENT)
            return cached;
        if (!isFresh(cachedAt, nowMs, MAX_AGE_MS))
            return null;
        return cached;
    }

    /** Whether a refresh is already running, so callers do not stack them up. */
    boolean isRefreshing() {
        return refreshing;
    }

    /** Claims the single refresh slot. Returns false when one is already running. */
    boolean beginRefresh() {
        if (refreshing)
            return false;
        refreshing = true;
        return true;
    }

    void endRefresh() {
        refreshing = false;
    }

    /** Runs the blocking query. Called off the main thread. */
    GoldInputStatusClient.Result query() {
        return client.query();
    }

    /**
     * Records an answer. A query that came back after {@link #MAX_AGE_MS} of its
     * own — a wedged {@code su} that finally returned — is not worth keeping.
     */
    void store(GoldInputStatusClient.Result result, long queriedAtMs, long nowMs) {
        if (result != null && result.state == GoldInputStatusClient.State.ABSENT) {
            cached = result;
            cachedAt = nowMs;
            return;
        }
        if (!isFresh(queriedAtMs, nowMs, MAX_AGE_MS))
            return;
        cached = result;
        cachedAt = nowMs;
    }

    static boolean isFresh(long atMs, long nowMs, long maxAgeMs) {
        if (atMs <= 0L || nowMs < atMs)
            return false;
        return nowMs - atMs <= maxAgeMs;
    }
}

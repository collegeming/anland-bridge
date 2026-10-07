package com.anland.consumer;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import java.util.Arrays;
import java.util.Collections;
import java.util.concurrent.atomic.AtomicInteger;

import org.junit.Test;

/**
 * The occupancy cache. Its whole reason to exist is that the immersive toggle
 * must not wait for an {@code su} round trip, so what matters here is that a
 * press always finds an answer when one is recent, and never blocks when there
 * is none.
 */
public class GoldStatusCacheTest {
    private static final long NOW = 1_000_000L;

    private static GoldInputStatusClient.Result verified(String... nodes) {
        java.util.List<String> list = Arrays.asList(nodes);
        return GoldInputStatusClient.Result.verified(new GoldInputStatusClient.Snapshot(
                "boot", 1L, "0123456789abcdef0123456789abcdef", list, list));
    }

    /** Never queries; the cache must not need it to answer. */
    private static GoldInputStatusClient neverQueried(AtomicInteger calls) {
        return new GoldInputStatusClient(new GoldController((command, timeout) -> {
            calls.incrementAndGet();
            return new SuCommand.CommandResult(1, "", "", false, false, false);
        }, "/nonexistent/bin/keyboardctl", new java.security.SecureRandom()));
    }

    @Test
    public void aColdCacheAnswersNothingWithoutQuerying() {
        AtomicInteger calls = new AtomicInteger();
        GoldStatusCache cache = new GoldStatusCache(neverQueried(calls));

        assertNull(cache.fresh(NOW));
        assertEquals(0, calls.get());
    }

    @Test
    public void aRecentAnswerIsServed() {
        GoldStatusCache cache = new GoldStatusCache(neverQueried(new AtomicInteger()));
        cache.store(verified("event3"), NOW, NOW);

        GoldInputStatusClient.Result cached = cache.fresh(NOW + 100L);
        assertEquals(GoldInputStatusClient.State.VERIFIED, cached.state);
        assertEquals(Collections.singletonList("event3"), cached.snapshot.nodes);
    }

    @Test
    public void anAnswerAgesOutRatherThanBeingTrustedForever() {
        GoldStatusCache cache = new GoldStatusCache(neverQueried(new AtomicInteger()));
        cache.store(verified("event3"), NOW, NOW);

        assertTrue(cache.fresh(NOW + GoldStatusCache.MAX_AGE_MS) != null);
        assertNull(cache.fresh(NOW + GoldStatusCache.MAX_AGE_MS + 1));
    }

    @Test
    public void aQueryThatTookTooLongIsNotWorthKeeping() {
        GoldStatusCache cache = new GoldStatusCache(neverQueried(new AtomicInteger()));

        // Started long ago, landed now: by the time it arrived the world may have
        // moved on, so it is dropped rather than believed.
        cache.store(verified("event3"), NOW - GoldStatusCache.MAX_AGE_MS - 1, NOW);

        assertNull(cache.fresh(NOW));
    }

    @Test
    public void clockGoingBackwardsIsNotTreatedAsFresh() {
        assertFalse(GoldStatusCache.isFresh(NOW, NOW - 1, GoldStatusCache.MAX_AGE_MS));
        assertFalse(GoldStatusCache.isFresh(0L, NOW, GoldStatusCache.MAX_AGE_MS));
    }

    @Test
    public void onlyOneRefreshRunsAtATime() {
        GoldStatusCache cache = new GoldStatusCache(neverQueried(new AtomicInteger()));

        assertTrue(cache.beginRefresh());
        assertTrue(cache.isRefreshing());
        // A second press must not stack another su call on top of the first.
        assertFalse(cache.beginRefresh());

        cache.endRefresh();
        assertFalse(cache.isRefreshing());
        assertTrue(cache.beginRefresh());
    }

    @Test
    public void anUnknownAnswerIsStillAnAnswer() {
        GoldStatusCache cache = new GoldStatusCache(neverQueried(new AtomicInteger()));
        cache.store(GoldInputStatusClient.Result.unknown(), NOW, NOW);

        // It has to be served, not swallowed: the caller warns on UNKNOWN, and a
        // cache that hid it would silently turn "could not check" into "nothing
        // is held".
        assertEquals(GoldInputStatusClient.State.UNKNOWN, cache.fresh(NOW).state);
    }

    @Test
    public void anAbsentAnswerIsAlsoKept() {
        GoldStatusCache cache = new GoldStatusCache(neverQueried(new AtomicInteger()));
        cache.store(GoldInputStatusClient.Result.absent(), NOW, NOW);

        assertEquals(GoldInputStatusClient.State.ABSENT, cache.fresh(NOW).state);
    }

    @Test
    public void anAbsentAnswerDoesNotBecomeUnknownWhenItAges() {
        GoldStatusCache cache = new GoldStatusCache(neverQueried(new AtomicInteger()));
        cache.store(GoldInputStatusClient.Result.absent(), NOW, NOW);

        assertEquals(GoldInputStatusClient.State.ABSENT,
                cache.fresh(NOW + GoldStatusCache.MAX_AGE_MS + 1).state);
    }
}

#!/usr/bin/env python3
"""Device-free contract model for exceptional lifetime interleavings.

These tests validate the intended ownership/state contract, not live kernel
locking or DMA behavior. persistent_logic_test.c tests the actual shared C
predicates. Hardware integration must additionally verify SG placement and
completion/cleanup counters; no real DMA fault injection belongs in this test.
"""
import itertools
import unittest


class File:
    identifiers = itertools.count(1)

    def __init__(self):
        self.handles = {}
        self.registrations = []
        self.fds = 1
        self.closed = False
        self.uncertain = 0
        self.quarantine = False

    def register(self, backing, fd):
        reg = Registration(self, backing, fd, next(self.identifiers))
        self.handles[reg.handle] = reg
        self.registrations.append(reg)
        return reg

    def lookup(self, handle):
        return self.handles[handle]

    def duplicate(self):
        self.fds += 1
        return self

    def unregister(self, handle):
        reg = self.lookup(handle)
        if reg.active:
            raise BlockingIOError()
        del self.handles[handle]
        reg.retired = True
        reg.cleanup()

    def close(self):
        self.fds -= 1
        if self.fds:
            return
        self.closed = True
        for reg in list(self.handles.values()):
            reg.retired = True
            reg.cleanup()
        self.handles.clear()

    def stop_allowed(self):
        return self.closed and all(r.destroyed for r in self.registrations)


class Registration:
    def __init__(self, owner, backing, fd, handle):
        self.owner, self.backing, self.fd, self.handle = owner, backing, fd, handle
        self.mapping = True
        self.generation = 1
        self.active = False
        self.retired = False
        self.destroyed = False
        self.stale = False
        self.fence_signaled = False
        self.wait_uncertain = False
        self.callback_error = False

    def start(self, length=4096):
        if self.retired or self.active or self.owner.quarantine or self.owner.uncertain:
            raise BlockingIOError()
        if not 0 < length <= 8192:
            raise ValueError()
        if not self.mapping:
            self.mapping = True
            self.generation += 1
        self.stale = False
        self.active = True
        self.fence_signaled = False
        self.length = length

    def move(self):
        self.stale = True
        if not self.active:
            self.mapping = False

    def wait_ends(self):
        if self.active and not self.wait_uncertain:
            self.wait_uncertain = True
            self.owner.uncertain += 1

    def complete(self, count=None, error=0):
        count = self.length if count is None else count
        if self.callback_error:
            return  # a failed callback is not converted into a fabricated success
        if error or count != self.length:
            self.callback_error = True
            self.owner.quarantine = True
            return
        self.fence_signaled = True
        self.active = False
        if self.wait_uncertain:
            self.owner.uncertain -= 1
            self.wait_uncertain = False
        if self.stale:
            self.mapping = False
        self.cleanup()

    def cleanup(self):
        if self.retired and not self.active:
            self.mapping = False
            self.destroyed = True


class LifetimeTests(unittest.TestCase):
    def test_fd_reuse_and_foreign_handles(self):
        a, b = File(), File()
        original = object()
        ar = a.register(original, 8)
        br = b.register(object(), 8)
        self.assertIs(ar.backing, original)
        self.assertNotEqual(ar.handle, br.handle)
        with self.assertRaises(KeyError):
            b.lookup(ar.handle)
        a.unregister(ar.handle)
        ar2 = a.register(object(), 8)
        self.assertGreater(ar2.handle, ar.handle)
        with self.assertRaises(KeyError):
            a.lookup(ar.handle)

    def test_dup_shares_open_description(self):
        a = File()
        r = a.register(object(), 4)
        duplicate = a.duplicate()
        a.close()
        self.assertIs(duplicate.lookup(r.handle), r)
        self.assertFalse(a.closed)
        duplicate.close()
        self.assertTrue(r.destroyed)
        self.assertTrue(a.stop_allowed())

    def test_registration_failure_unwinds_previous_registrations(self):
        a = File()
        registered = [a.register(object(), fd) for fd in range(4)]
        # Fifth register fails before publication: callers unwind existing IDs.
        for reg in reversed(registered):
            a.unregister(reg.handle)
        self.assertFalse(a.handles)
        self.assertTrue(all(r.destroyed for r in registered))
        a.close()
        self.assertTrue(a.stop_allowed())

    def test_timeout_and_signal_retain_until_late_success(self):
        for event in ('timeout', 'signal'):
            with self.subTest(event=event):
                a = File()
                r = a.register(object(), 4)
                other = a.register(object(), 5)
                r.start()
                r.wait_ends()
                self.assertTrue(r.mapping and r.active)
                self.assertFalse(r.fence_signaled)
                with self.assertRaises(BlockingIOError):
                    other.start()
                with self.assertRaises(BlockingIOError):
                    a.unregister(r.handle)
                r.complete()
                self.assertTrue(r.fence_signaled)
                self.assertEqual(a.uncertain, 0)
                other.start()
                other.complete()
                a.close()
                self.assertTrue(a.stop_allowed())

    def test_close_during_dma_and_move(self):
        a = File()
        r = a.register(object(), 4)
        r.start()
        a.close()
        self.assertTrue(r.retired and r.active and r.mapping)
        self.assertFalse(a.stop_allowed())
        r.move()
        self.assertTrue(r.mapping)  # exporter waits published unsignaled fence
        r.complete()
        self.assertTrue(r.fence_signaled and r.destroyed)
        self.assertTrue(a.stop_allowed())

    def test_move_without_dma_and_remap(self):
        a = File()
        r = a.register(object(), 4)
        r.move()
        self.assertFalse(r.mapping)
        r.start(1)
        self.assertEqual(r.generation, 2)
        r.complete()
        r.start(8192)
        r.complete()
        self.assertEqual(r.generation, 2)  # stable reuse never remaps
        a.close()

    def test_uncertain_error_never_signals_or_unmaps(self):
        for count, error in ((0, -6), (1024, 0), (4096, -5)):
            with self.subTest(count=count, error=error):
                a = File()
                r = a.register(object(), 4)
                r.start()
                r.complete(count, error)
                a.close()
                r.move()
                self.assertTrue(a.quarantine and r.mapping and r.active)
                self.assertFalse(r.fence_signaled or r.destroyed or a.stop_allowed())

    def test_independent_queues_and_variable_transfer_length(self):
        queues = [File() for _ in range(12)]
        regs = [q.register(object(), 10) for q in queues]
        for i, r in enumerate(regs):
            r.start(i + 1)
        for r in reversed(regs):
            r.complete()
        for q in queues:
            q.close()
        self.assertTrue(all(q.stop_allowed() for q in queues))
        for length in (0, -1, 8193):
            q = File()
            r = q.register(object(), 1)
            with self.assertRaises(ValueError):
                r.start(length)
            q.close()

    def test_completion_timeout_race_both_orders(self):
        for order in ('completion-first', 'timeout-first'):
            a = File()
            r = a.register(object(), 4)
            r.start()
            if order == 'completion-first':
                r.complete()
                r.wait_ends()
            else:
                r.wait_ends()
                r.complete()
            self.assertEqual(a.uncertain, 0)
            a.close()
            self.assertTrue(a.stop_allowed())


if __name__ == '__main__':
    unittest.main(verbosity=2)

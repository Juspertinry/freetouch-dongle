# Pairing CV1 Touch controllers

A CV1 Touch remembers one host: its headset, or a FreeTouch dongle. Pairing it to the dongle
replaces its pairing with a headset (pair it with the Oculus app again to go back), and nothing else
on the controller changes: its firmware is never written.

The controllers must run their standard Touch firmware, which every controller that has worked with
a Rift does.

## Pair one controller

1. Run `freetouch-cli pair`. The dongle pauses its link to the controllers it already has and
   listens for up to 60 seconds.
2. Put the controller in pairing mode:
   - **left**: hold **Menu + Y**
   - **right**: hold **Oculus + B**

   and keep holding until `freetouch-cli` reports the result.
3. `freetouch-cli` prints `paired`, and the dongle goes back to polling every paired controller,
   the new one included.

If it says `no controller in pairing mode within 60 s`, run it again and put the controller in
pairing mode sooner. If it says `pairing failed`, bring the controller closer to the dongle and try
again. A failed attempt changes nothing on the controller.

An app with dongle support pairs the same way: it sends the pair command (`P`) and watches the
pairing status records (docs/PROTOCOL.md).

## Reply slots, and more than two controllers

The dongle polls every 2 ms and each controller answers at its own fixed delay after the poll, its
**reply slot**. Pairing writes that delay into the controller. Without one given, the dongle picks
by hand: 1000 us for a left controller and 1250 us for a right one, so a plain pair of controllers
needs nothing more.

Two controllers of the **same hand** need different slots, or their replies collide. Give the
second one its own delay when you pair it:

| Controller | Pair with |
|---|---|
| first left | `freetouch-cli pair` (1000 us) |
| first right | `freetouch-cli pair` (1250 us) |
| second left or right | `freetouch-cli pair 1500` |
| fourth | `freetouch-cli pair 750` |

Delays from 400 to 1620 us are accepted; the four above are spaced so that every reply fits
between polls with room to spare. Up to four controllers have been run together (two per hand, for
full-body tracking).

Two controllers of one hand share the hand's radio session. They are told apart by their reply slot
(every input record carries it), but a haptic command goes to the session, so it buzzes both.

## Checking

`freetouch-cli monitor` lists each controller once a second by hand and slot, with its report
rate, signal strength and live inputs. Press buttons to see which controller is which.

Reports that do not decrypt are counted separately. A few are radio corruption that got past the
packet checksum and are dropped harmlessly; a steady stream means something is wrong with the link
and is worth a bug report with `freetouch-cli monitor` output.

A controller paired to a headset or to another dongle listens on that host's radio address, so it
does not appear here at all until it is paired to this dongle.

## After a firmware update

Nothing to do: updates keep the dongle's identity and so every pairing (docs/INSTALL.md). Only a
chip erase or a bootloader update can clear it; then pair every controller again.

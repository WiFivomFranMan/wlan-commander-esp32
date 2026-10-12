/* Generator, sniffer packet pool and SDR are exclusive owners of this bank. */
SECTIONS {
  .wlc_rftest 0x40820000 (NOLOAD) : ALIGN(16) {
    _wlc_rftest_start = .;
    KEEP(*(.wlc_rftest.*))
    _wlc_rftest_end = .;
  }
}
ASSERT(_wlc_rftest_start == 0x40820000, "RF TX scratch bank moved");
ASSERT(_wlc_rftest_end <= 0x40830000, "RF TX scratch exceeds shared lower bank");

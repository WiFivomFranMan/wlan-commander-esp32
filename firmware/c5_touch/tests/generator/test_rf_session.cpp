#include "rf_approval.hpp"
#include <cassert>
#include <iostream>
using wlc::rf::Session;
int main() {
    Session s;
    assert(!s.active(1) && !s.allows(1,1));
    assert(!s.unlock(false,true,0)); assert(!s.unlock(true,false,0));
    assert(!s.unlock(true,true,0,900001)); assert(!s.unlock(true,true,0,0));
    assert(s.unlock(true,true,100,1000)); assert(s.remaining(100)==1000);
    assert(!s.unlock(true,true,500,1000)); assert(s.deadline==1100);
    assert(s.allows(100,1000) && !s.allows(101,1000) && !s.allows(100,0));
    assert(s.active(1099) && !s.active(1100) && !s.allows(1100,1));
    s.lock(); assert(!s.active(0));
    assert(s.unlock(true,true,0xfffffff0,1000)); assert(s.remaining(0xfffffff0)==1000);
    assert(s.active(983) && !s.active(984));
    Session rebooted; assert(!rebooted.active(1));
    std::cout << "RF approval acknowledgement, expiry, budget, nonrenewal and rollover checks passed\n";
}

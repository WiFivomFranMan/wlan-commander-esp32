CXX ?= clang++
CXXFLAGS = -std=c++17 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
INCLUDES = -I../../components/wlc_generator/include -I../../firmware/wlc_sensor/main
.PHONY: check clean
check: test_generator test_rf_session
	./test_generator
	./test_rf_session
test_generator: test_generator.cpp ../../components/wlc_generator/include/wlc_generator.hpp ../../components/wlc_generator/include/wlc_radar.hpp ../../firmware/wlc_sensor/main/generator_view.h ../../firmware/wlc_sensor/main/generator_policy.h
	$(CXX) $(CXXFLAGS) $(INCLUDES) $< -o $@
test_rf_session: test_rf_session.cpp ../../components/wlc_rf_approval/include/rf_approval.hpp
	$(CXX) $(CXXFLAGS) -I../../components/wlc_rf_approval/include $< -o $@
clean:
	rm -f test_generator test_rf_session

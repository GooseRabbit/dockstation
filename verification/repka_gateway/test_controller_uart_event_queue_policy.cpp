#include "controller_uart_event_queue_policy.hpp"
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <iostream>
using namespace dockstation::firmware::esp32;
namespace {
int checks=0, failures=0;
void expect(bool ok,const char* message){++checks;if(ok)std::cout<<"PASS - "<<message<<'\n';else{std::cerr<<"FAIL - "<<message<<'\n';++failures;}}
struct EventQueue {
 std::deque<ControllerUartEventKind> events{};
 bool try_receive(ControllerUartEventKind& out) noexcept {if(events.empty())return false;out=events.front();events.pop_front();return true;}
 bool has_pending()const noexcept{return !events.empty();}
};
}
int main(){
 EventQueue backlog{};for(std::size_t i=0;i<11U;++i)backlog.events.push_back(ControllerUartEventKind::Data);
 auto first=drain_controller_uart_event_batch([&](auto& e)noexcept{return backlog.try_receive(e);},[&]()noexcept{return backlog.has_pending();});
 expect(first.status==ControllerUartEventDrainStatus::MorePending&&first.processed_events==8U&&backlog.events.size()==3U,"bounds each poll to eight events and reports backlog");
 auto second=drain_controller_uart_event_batch([&](auto& e)noexcept{return backlog.try_receive(e);},[&]()noexcept{return backlog.has_pending();});
 expect(second.status==ControllerUartEventDrainStatus::Drained&&second.processed_events==3U&&backlog.events.empty(),"drains deferred events on the next poll without dropping them");
 EventQueue exact{};for(std::size_t i=0;i<8U;++i)exact.events.push_back(ControllerUartEventKind::Informational);
 auto exact_result=drain_controller_uart_event_batch([&](auto& e)noexcept{return exact.try_receive(e);},[&]()noexcept{return exact.has_pending();});
 expect(exact_result.status==ControllerUartEventDrainStatus::Drained&&exact_result.processed_events==8U,"does not report false backlog when queue empties at batch limit");
 EventQueue overflow{};for(std::size_t i=0;i<7U;++i)overflow.events.push_back(ControllerUartEventKind::Data);overflow.events.push_back(ControllerUartEventKind::FifoOverflow);overflow.events.push_back(ControllerUartEventKind::Data);
 auto overflow_result=drain_controller_uart_event_batch([&](auto& e)noexcept{return overflow.try_receive(e);},[&]()noexcept{return overflow.has_pending();});
 expect(overflow_result.status==ControllerUartEventDrainStatus::Fault&&overflow_result.fault==ControllerUartEventFault::FifoOverflow&&overflow_result.processed_events==8U&&overflow.events.size()==1U,"treats UART FIFO overflow as fatal instead of ordinary backlog");
 EventQueue framing{};framing.events.push_back(ControllerUartEventKind::FrameError);
 auto framing_result=drain_controller_uart_event_batch([&](auto& e)noexcept{return framing.try_receive(e);},[&]()noexcept{return framing.has_pending();});
 expect(framing_result.status==ControllerUartEventDrainStatus::Fault&&framing_result.fault==ControllerUartEventFault::FrameError,"stops immediately on UART framing error");
 EventQueue empty{};
 auto empty_result=drain_controller_uart_event_batch([&](auto& e)noexcept{return empty.try_receive(e);},[&]()noexcept{return empty.has_pending();});
 expect(empty_result.status==ControllerUartEventDrainStatus::Drained&&empty_result.processed_events==0U,"handles an empty queue without waiting");
 std::cout<<"Checks: "<<checks<<", failures: "<<failures<<'\n';return failures==0?EXIT_SUCCESS:EXIT_FAILURE;
}

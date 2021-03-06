
//          Copyright Oliver Kowalke 2013.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "boost/fiber/context.hpp"

#include <cstdlib>
#include <mutex>
#include <new>

#include "boost/fiber/exceptions.hpp"
#include "boost/fiber/scheduler.hpp"

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

namespace boost {
namespace fibers {

static intrusive_ptr< context > make_dispatcher_context( scheduler * sched) {
    BOOST_ASSERT( nullptr != sched);
    default_stack salloc; // use default satck-size
    boost::context::stack_context sctx = salloc.allocate();
#if defined(BOOST_NO_CXX14_CONSTEXPR) || defined(BOOST_NO_CXX11_STD_ALIGN)
    // reserve space for control structure
    const std::size_t size = sctx.size - sizeof( context);
    void * sp = static_cast< char * >( sctx.sp) - sizeof( context);
#else
    constexpr std::size_t func_alignment = 64; // alignof( context);
    constexpr std::size_t func_size = sizeof( context);
    // reserve space on stack
    void * sp = static_cast< char * >( sctx.sp) - func_size - func_alignment;
    // align sp pointer
    std::size_t space = func_size + func_alignment;
    sp = std::align( func_alignment, func_size, sp, space);
    BOOST_ASSERT( nullptr != sp);
    // calculate remaining size
    const std::size_t size = sctx.size - ( static_cast< char * >( sctx.sp) - static_cast< char * >( sp) );
#endif
    // placement new of context on top of fiber's stack
    return intrusive_ptr< context >{
        ::new ( sp) context{
                dispatcher_context,
                boost::context::preallocated{ sp, size, sctx },
                salloc,
                sched } };
}

// schwarz counter
struct context_initializer {
    static thread_local context *   active_;
    static thread_local std::size_t counter_;

    context_initializer() {
        if ( 0 == counter_++) {
# if defined(BOOST_NO_CXX14_CONSTEXPR) || defined(BOOST_NO_CXX11_STD_ALIGN)
            // allocate memory for main context and scheduler
            constexpr std::size_t size = sizeof( context) + sizeof( scheduler);
            void * vp = std::malloc( size);
            if ( nullptr == vp) {
                throw std::bad_alloc{};
            }
            // main fiber context of this thread
            context * main_ctx = ::new ( vp) context{ main_context };
            // scheduler of this thread
            scheduler * sched = ::new ( static_cast< char * >( vp) + sizeof( context) ) scheduler{};
            // attach main context to scheduler
            sched->attach_main_context( main_ctx);
            // create and attach dispatcher context to scheduler
            sched->attach_dispatcher_context( make_dispatcher_context( sched) );
            // make main context to active context
            active_ = main_ctx;
# else
            constexpr std::size_t alignment = 64; // alignof( capture_t);
            constexpr std::size_t ctx_size = sizeof( context);
            constexpr std::size_t sched_size = sizeof( scheduler);
            constexpr std::size_t size = 2 * alignment + ctx_size + sched_size;
            void * vp = std::malloc( size);
            if ( nullptr == vp) {
                throw std::bad_alloc{};
            }
            // reserve space for shift
            void * vp1 = static_cast< char * >( vp) + sizeof( int); 
            // align context pointer
            std::size_t space = ctx_size + alignment;
            vp1 = std::align( alignment, ctx_size, vp1, space);
            // reserves space for integer holding shifted size
            int * shift = reinterpret_cast< int * >( static_cast< char * >( vp1) - sizeof( int) );
            // store shifted size in fornt of context
            * shift = static_cast< char * >( vp1) - static_cast< char * >( vp);
            // main fiber context of this thread
            context * main_ctx = ::new ( vp1) context{ main_context };
            vp1 = static_cast< char * >( vp1) + ctx_size;
            // align scheduler pointer
            space = sched_size + alignment;
            vp1 = std::align( alignment, sched_size, vp1, space);
            // scheduler of this thread
            scheduler * sched = ::new ( vp1) scheduler{};
            // attach main context to scheduler
            sched->attach_main_context( main_ctx);
            // create and attach dispatcher context to scheduler
            sched->attach_dispatcher_context( make_dispatcher_context( sched) );
            // make main context to active context
            active_ = main_ctx;
# endif
        }
    }

    ~context_initializer() {
        if ( 0 == --counter_) {
            context * main_ctx = active_;
            BOOST_ASSERT( main_ctx->is_context( type::main_context) );
            scheduler * sched = main_ctx->get_scheduler();
            sched->~scheduler();
            main_ctx->~context();
# if defined(BOOST_NO_CXX14_CONSTEXPR) || defined(BOOST_NO_CXX11_STD_ALIGN)
            std::free( main_ctx);
# else
            int * shift = reinterpret_cast< int * >( reinterpret_cast< char * >( main_ctx) - sizeof( int) );
            void * vp = reinterpret_cast< char * >( main_ctx) - ( * shift);
            std::free( vp);
# endif
        }
    }
};

// zero-initialization
thread_local context * context_initializer::active_;
thread_local std::size_t context_initializer::counter_;

context *
context::active() noexcept {
#if (BOOST_EXECUTION_CONTEXT==1)
    // initialized the first time control passes; per thread
    thread_local static boost::context::detail::activation_record_initializer rec_initializer;
#endif
    // initialized the first time control passes; per thread
    thread_local static context_initializer ctx_initializer;
    return context_initializer::active_;
}

void
context::reset_active() noexcept {
    context_initializer::active_ = nullptr;
}

#if (BOOST_EXECUTION_CONTEXT==1)
void
context::resume_( detail::data_t & d) noexcept {
    detail::data_t * dp = static_cast< detail::data_t * >( ctx_( & d) );
    if ( nullptr != dp->lk) {
        dp->lk->unlock();
    } else if ( nullptr != dp->ctx) {
        active()->schedule_( dp->ctx);
    }
}
#else
void
context::resume_( detail::data_t & d) noexcept {
    boost::context::continuation c = c_.resume( & d);
    if ( c) {
        detail::data_t * dp = c.get_data< detail::data_t * >();
        if ( nullptr != dp) {
            dp->from->c_ = std::move( c);
            if ( nullptr != dp->lk) {
                dp->lk->unlock();
            } else if ( nullptr != dp->ctx) {
                active()->schedule_( dp->ctx);
            }
        }
    }
}
#endif

void
context::schedule_( context * ctx) noexcept {
    get_scheduler()->schedule( ctx);
}

// main fiber context
context::context( main_context_t) noexcept :
    use_count_{ 1 }, // allocated on main- or thread-stack
#if (BOOST_EXECUTION_CONTEXT==1)
    ctx_{ boost::context::execution_context::current() },
    type_{ type::main_context },
    policy_{ launch::post } {
#else
    c_{},
    type_{ type::main_context },
    policy_{ launch::post } {
#endif
}

// dispatcher fiber context
context::context( dispatcher_context_t, boost::context::preallocated const& palloc,
                  default_stack const& salloc, scheduler * sched) :
#if (BOOST_EXECUTION_CONTEXT==1)
    ctx_{ std::allocator_arg, palloc, salloc,
          [this,sched](void * vp) noexcept {
              detail::data_t * dp = static_cast< detail::data_t * >( vp);
            if ( nullptr != dp->lk) {
                dp->lk->unlock();
            } else if ( nullptr != dp->ctx) {
                active()->schedule_( dp->ctx);
            }
            // execute scheduler::dispatch()
            sched->dispatch();
            // dispatcher context should never return from scheduler::dispatch()
            BOOST_ASSERT_MSG( false, "disatcher fiber already terminated");
          }},
    type_{ type::dispatcher_context },
    policy_{ launch::post }
{}
#else
    c_{},
    type_{ type::dispatcher_context },
    policy_{ launch::post }
{
    c_ = boost::context::callcc(
            std::allocator_arg, palloc, salloc,
            [this,sched](boost::context::continuation && c) noexcept {
                c = c.resume();
                detail::data_t * dp = c.get_data< detail::data_t * >(); 
                // update continuation of calling fiber
                dp->from->c_ = std::move( c);
                if ( nullptr != dp->lk) {
                    dp->lk->unlock();
                } else if ( nullptr != dp->ctx) {
                    active()->schedule_( dp->ctx);
                }
                // execute scheduler::dispatch()
                return sched->dispatch();
            });

}
#endif

context::~context() {
    // protect for concurrent access
    std::unique_lock< detail::spinlock > lk{ splk_ };
    BOOST_ASSERT( ! ready_is_linked() );
    BOOST_ASSERT( ! remote_ready_is_linked() );
    BOOST_ASSERT( ! sleep_is_linked() );
    BOOST_ASSERT( ! wait_is_linked() );
    if ( is_context( type::dispatcher_context) ) {
        // dispatcher-context is resumed by main-context
        // while the scheduler is deconstructed
#ifdef BOOST_DISABLE_ASSERTS
        wait_queue_.pop_front();
#else
        context * ctx = & wait_queue_.front();
        wait_queue_.pop_front();
        BOOST_ASSERT( ctx->is_context( type::main_context) );
        BOOST_ASSERT( nullptr == active() );
#endif
    }
    BOOST_ASSERT( wait_queue_.empty() );
    delete properties_;
}

context::id
context::get_id() const noexcept {
    return id{ const_cast< context * >( this) };
}

void
context::resume() noexcept {
    context * prev = this;
    // context_initializer::active_ will point to `this`
    // prev will point to previous active context
    std::swap( context_initializer::active_, prev);
#if (BOOST_EXECUTION_CONTEXT==1)
    detail::data_t d{};
#else
    detail::data_t d{ prev };
#endif
    resume_( d);
}

void
context::resume( detail::spinlock_lock & lk) noexcept {
    context * prev = this;
    // context_initializer::active_ will point to `this`
    // prev will point to previous active context
    std::swap( context_initializer::active_, prev);
#if (BOOST_EXECUTION_CONTEXT==1)
    detail::data_t d{ & lk };
#else
    detail::data_t d{ & lk, prev };
#endif
    resume_( d);
}

void
context::resume( context * ready_ctx) noexcept {
    context * prev = this;
    // context_initializer::active_ will point to `this`
    // prev will point to previous active context
    std::swap( context_initializer::active_, prev);
#if (BOOST_EXECUTION_CONTEXT==1)
    detail::data_t d{ ready_ctx };
#else
    detail::data_t d{ ready_ctx, prev };
#endif
    resume_( d);
}

void
context::suspend() noexcept {
    get_scheduler()->suspend();
}

void
context::suspend( detail::spinlock_lock & lk) noexcept {
    get_scheduler()->suspend( lk);
}

void
context::join() {
    // get active context
    context * active_ctx = context::active();
    // protect for concurrent access
    std::unique_lock< detail::spinlock > lk{ splk_ };
    // wait for context which is not terminated
    if ( ! terminated_) {
        // push active context to wait-queue, member
        // of the context which has to be joined by
        // the active context
        active_ctx->wait_link( wait_queue_);
        // suspend active context
        active_ctx->get_scheduler()->suspend( lk);
        // active context resumed
        BOOST_ASSERT( context::active() == active_ctx);
    }
}

void
context::yield() noexcept {
    // yield active context
    get_scheduler()->yield( context::active() );
}

#if (BOOST_EXECUTION_CONTEXT==1)
void
context::terminate() noexcept {
    // protect for concurrent access
    std::unique_lock< detail::spinlock > lk{ splk_ };
    // mark as terminated
    terminated_ = true;
    // notify all waiting fibers
    while ( ! wait_queue_.empty() ) {
        context * ctx = & wait_queue_.front();
        // remove fiber from wait-queue
        wait_queue_.pop_front();
        // notify scheduler
        schedule( ctx);
    }
    BOOST_ASSERT( wait_queue_.empty() );
    // release fiber-specific-data
    for ( fss_data_t::value_type & data : fss_data_) {
        data.second.do_cleanup();
    }
    fss_data_.clear();
    // switch to another context
    get_scheduler()->terminate( lk, this);
}
#else
boost::context::continuation
context::suspend_with_cc() noexcept {
    context * prev = this;
    // context_initializer::active_ will point to `this`
    // prev will point to previous active context
    std::swap( context_initializer::active_, prev);
    detail::data_t d{ prev };
    // context switch
    return c_.resume( & d);
}

boost::context::continuation
context::terminate() noexcept {
    // protect for concurrent access
    std::unique_lock< detail::spinlock > lk{ splk_ };
    // mark as terminated
    terminated_ = true;
    // notify all waiting fibers
    while ( ! wait_queue_.empty() ) {
        context * ctx = & wait_queue_.front();
        // remove fiber from wait-queue
        wait_queue_.pop_front();
        // notify scheduler
        schedule( ctx);
    }
    BOOST_ASSERT( wait_queue_.empty() );
    // release fiber-specific-data
    for ( fss_data_t::value_type & data : fss_data_) {
        data.second.do_cleanup();
    }
    fss_data_.clear();
    // switch to another context
    return get_scheduler()->terminate( lk, this);
#endif
}

bool
context::wait_until( std::chrono::steady_clock::time_point const& tp) noexcept {
    BOOST_ASSERT( nullptr != get_scheduler() );
    BOOST_ASSERT( this == active());
    return get_scheduler()->wait_until( this, tp);
}

bool
context::wait_until( std::chrono::steady_clock::time_point const& tp,
                     detail::spinlock_lock & lk) noexcept {
    BOOST_ASSERT( nullptr != get_scheduler() );
    BOOST_ASSERT( this == active());
    return get_scheduler()->wait_until( this, tp, lk);
}

void
context::schedule( context * ctx) noexcept {
    //BOOST_ASSERT( nullptr != ctx);
    BOOST_ASSERT( this != ctx);
    BOOST_ASSERT( nullptr != get_scheduler() );
    BOOST_ASSERT( nullptr != ctx->get_scheduler() );
#if ! defined(BOOST_FIBERS_NO_ATOMICS)
    // FIXME: comparing scheduler address' must be synchronized?
    //        what if ctx is migrated between threads
    //        (other scheduler assigned)
    if ( scheduler_ == ctx->get_scheduler() ) {
        // local
        schedule_( ctx);
    } else {
        // remote
        ctx->get_scheduler()->schedule_from_remote( ctx);
    }
#else
    BOOST_ASSERT( get_scheduler() == ctx->get_scheduler() );
    schedule_( ctx);
#endif
}

void *
context::get_fss_data( void const * vp) const {
    uintptr_t key = reinterpret_cast< uintptr_t >( vp);
    fss_data_t::const_iterator i = fss_data_.find( key);
    return fss_data_.end() != i ? i->second.vp : nullptr;
}

void
context::set_fss_data( void const * vp,
                       detail::fss_cleanup_function::ptr_t const& cleanup_fn,
                       void * data,
                       bool cleanup_existing) {
    BOOST_ASSERT( cleanup_fn);
    uintptr_t key = reinterpret_cast< uintptr_t >( vp);
    fss_data_t::iterator i = fss_data_.find( key);
    if ( fss_data_.end() != i) {
        if( cleanup_existing) {
            i->second.do_cleanup();
        }
        if ( nullptr != data) {
            fss_data_.insert(
                    i,
                    std::make_pair(
                        key,
                        fss_data{ data, cleanup_fn } ) );
        } else {
            fss_data_.erase( i);
        }
    } else {
        fss_data_.insert(
            std::make_pair(
                key,
                fss_data{ data, cleanup_fn } ) );
    }
}

void
context::set_properties( fiber_properties * props) noexcept {
    delete properties_;
    properties_ = props;
}

bool
context::worker_is_linked() const noexcept {
    return worker_hook_.is_linked();
}

bool
context::ready_is_linked() const noexcept {
    return ready_hook_.is_linked();
}

bool
context::remote_ready_is_linked() const noexcept {
    return remote_ready_hook_.is_linked();
}

bool
context::sleep_is_linked() const noexcept {
    return sleep_hook_.is_linked();
}

bool
context::terminated_is_linked() const noexcept {
    return terminated_hook_.is_linked();
}

bool
context::wait_is_linked() const noexcept {
    return wait_hook_.is_linked();
}

void
context::worker_unlink() noexcept {
    BOOST_ASSERT( worker_is_linked() );
    worker_hook_.unlink();
}

void
context::ready_unlink() noexcept {
    BOOST_ASSERT( ready_is_linked() );
    ready_hook_.unlink();
}

void
context::sleep_unlink() noexcept {
    BOOST_ASSERT( sleep_is_linked() );
    sleep_hook_.unlink();
}

void
context::detach() noexcept {
    BOOST_ASSERT( context::active() != this);
    get_scheduler()->detach_worker_context( this);
}

void
context::attach( context * ctx) noexcept {
    BOOST_ASSERT( nullptr != ctx);
    get_scheduler()->attach_worker_context( ctx);
}

}}

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

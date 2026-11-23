/*
-----------------------------------------------------------------------------
This source file is part of OGRE-Next
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2018 Torus Knot Software Ltd
Copyright (c) 2026 Jahshaka (this file: the jahshaka/ogre-next fork, ASYNC-SHADERS-1)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/

#include "OgreStableHeaders.h"

#include "OgreHlmsAsyncCompiler.h"

#include "OgreException.h"
#include "OgreHlms.h"
#include "OgreLogManager.h"
#include "OgreStringConverter.h"

#include <algorithm>
#include <chrono>
#include <cerrno>

#if OGRE_PLATFORM == OGRE_PLATFORM_LINUX
#    include <sys/resource.h>
#    include <sys/syscall.h>
#    include <unistd.h>
#endif

namespace Ogre
{
    namespace
    {
        thread_local bool tlIsServiceThread = false;

        /// Runs one job and records its failure. Never throws: a compile error belongs to
        /// the permutation, not to whoever asked for it.
        void runJob( HlmsAsyncJob *job, size_t tid )
        {
            try
            {
                job->run( tid );
            }
            catch( Exception &e )
            {
                job->mFailed = true;
                LogManager::getSingleton().logMessage(
                    "HlmsAsyncCompiler: a permutation failed to build: " + e.getFullDescription(),
                    LML_CRITICAL );
            }
            catch( std::exception &e )
            {
                job->mFailed = true;
                LogManager::getSingleton().logMessage(
                    String( "HlmsAsyncCompiler: a permutation failed to build: " ) + e.what(),
                    LML_CRITICAL );
            }
            catch( ... )
            {
                job->mFailed = true;
                LogManager::getSingleton().logMessage(
                    "HlmsAsyncCompiler: a permutation failed to build (unknown exception)",
                    LML_CRITICAL );
            }
        }
    }  // namespace

    HlmsAsyncJob::~HlmsAsyncJob() {}
    //-----------------------------------------------------------------------------------
    HlmsAsyncCompiler::HlmsAsyncCompiler() :
        mStop( false ),
        mNumOutstanding( 0u ),
        mNumCompleted( 0u ),
        mNumFailed( 0u ),
        mThreadNice( 0 ),
        mPlaceholdersOnly( false )
    {
    }
    //-----------------------------------------------------------------------------------
    HlmsAsyncCompiler::~HlmsAsyncCompiler() { stopThreads(); }
    //-----------------------------------------------------------------------------------
    bool HlmsAsyncCompiler::isServiceThread() { return tlIsServiceThread; }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::threadMain( const uint32 threadIdx )
    {
        tlIsServiceThread = true;
#if OGRE_PLATFORM == OGRE_PLATFORM_LINUX
        if( mThreadNice > 0 )
        {
            // Per-thread niceness: PRIO_PROCESS with a TID addresses the thread alone.
            const id_t tid = static_cast<id_t>( syscall( SYS_gettid ) );
            errno = 0;
            const int current = getpriority( PRIO_PROCESS, tid );
            if( errno == 0 )
                setpriority( PRIO_PROCESS, tid, std::min( 19, current + mThreadNice ) );
        }
#endif
        const size_t tid = Hlms::kAsyncTidBase + threadIdx;

        std::unique_lock<std::mutex> lock( mMutex );
        while( true )
        {
            mWorkCv.wait( lock, [this] { return mStop || !mQueue.empty() || !mJahAheadQueue.empty(); } );
            if( mStop )
                break;

            // (Jahshaka) THE FRAME's OWN QUEUE FIRST; the ahead class only when it is empty
            HlmsAsyncJob *job;
            if( !mQueue.empty() )
            {
                job = mQueue.front();
                mQueue.pop_front();
            }
            else
            {
                job = mJahAheadQueue.front();
                mJahAheadQueue.pop_front();
                mJahAheadQueued.store( static_cast<uint32>( mJahAheadQueue.size() ), std::memory_order_relaxed );
                mJahAheadRun.fetch_add( 1u, std::memory_order_relaxed );
            }
            mRunning[threadIdx] = job;
            lock.unlock();

            // (Jahshaka) the test door _jahServiceDelayMs: the job is RUNNING (a join waits for it) and late.
            if( const uint32 delayMs = mJahServiceDelayMs.load( std::memory_order_relaxed ) )
                std::this_thread::sleep_for( std::chrono::milliseconds( delayMs ) );

            runJob( job, tid );

            lock.lock();
            mRunning[threadIdx] = 0;
            mFinished.push_back( job );
            mNumCompleted.fetch_add( 1u, std::memory_order_relaxed );
            if( job->mFailed )
                mNumFailed.fetch_add( 1u, std::memory_order_relaxed );
            mDoneCv.notify_all();
        }
    }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::finish( HlmsAsyncJob *job, const bool publishIt )
    {
        try
        {
            if( publishIt )
                job->publish();
            else
                job->discard();
        }
        catch( Exception &e )
        {
            LogManager::getSingleton().logMessage(
                "HlmsAsyncCompiler: publishing a permutation failed: " + e.getFullDescription(),
                LML_CRITICAL );
        }
        OGRE_DELETE job;
        // (Jahshaka) resolved BEFORE it leaves the outstanding count: a reader of both never sees it in neither
        mJahNumResolved.fetch_add( 1u, std::memory_order_relaxed );
        mNumOutstanding.fetch_sub( 1u, std::memory_order_relaxed );
    }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::stopThreads()
    {
        {
            std::lock_guard<std::mutex> lock( mMutex );
            mStop = true;
        }
        mWorkCv.notify_all();
        for( std::thread &t : mThreads )
            t.join();
        mThreads.clear();

        // Nothing runs any more: whatever is left is dropped.
        std::vector<HlmsAsyncJob *> dropped;
        {
            std::lock_guard<std::mutex> lock( mMutex );
            dropped.insert( dropped.end(), mQueue.begin(), mQueue.end() );
            dropped.insert( dropped.end(), mJahAheadQueue.begin(), mJahAheadQueue.end() );
            dropped.insert( dropped.end(), mFinished.begin(), mFinished.end() );
            mQueue.clear();
            mJahAheadQueue.clear();
            mJahAheadQueued.store( 0u, std::memory_order_relaxed );
            mFinished.clear();
            mRunning.clear();
            mStop = false;
        }
        for( HlmsAsyncJob *job : dropped )
            finish( job, false );
    }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::setNumThreads( uint32 numThreads, const int niceIncrement )
    {
        numThreads = std::min( numThreads, kMaxThreads );
        stopThreads();
        mThreadNice = niceIncrement;
        if( !numThreads )
            return;

        {
            std::lock_guard<std::mutex> lock( mMutex );
            mRunning.assign( numThreads, static_cast<HlmsAsyncJob *>( 0 ) );
        }
        mThreads.reserve( numThreads );
        for( uint32 i = 0u; i < numThreads; ++i )
            mThreads.emplace_back( &HlmsAsyncCompiler::threadMain, this, i );

        LogManager::getSingleton().logMessage( "HlmsAsyncCompiler: " +
                                               StringConverter::toString( numThreads ) +
                                               " shader compile thread(s) running" );
    }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::submit( HlmsAsyncJob *job, const bool urgent )
    {
        if( mThreads.empty() )
        {
            OGRE_DELETE job;
            OGRE_EXCEPT( Exception::ERR_INVALID_STATE, "The asynchronous shader compiler is not running",
                         "HlmsAsyncCompiler::submit" );
        }
        mNumOutstanding.fetch_add( 1u, std::memory_order_relaxed );
        {
            std::lock_guard<std::mutex> lock( mMutex );
            std::deque<HlmsAsyncJob *> &q = mJahAheadClass ? mJahAheadQueue : mQueue;
            if( urgent )
                q.push_front( job );
            else
                q.push_back( job );
            mJahAheadQueued.store( static_cast<uint32>( mJahAheadQueue.size() ), std::memory_order_relaxed );
        }
        mWorkCv.notify_one();
    }
    //-----------------------------------------------------------------------------------
    HlmsAsyncJob *HlmsAsyncCompiler::jahTakeQueued( const void *key )
    {
        for( std::deque<HlmsAsyncJob *> *q : { &mQueue, &mJahAheadQueue } )
        {
            for( std::deque<HlmsAsyncJob *>::iterator it = q->begin(); it != q->end(); ++it )
            {
                if( ( *it )->mKey == key )
                {
                    HlmsAsyncJob *job = *it;
                    q->erase( it );
                    mJahAheadQueued.store( static_cast<uint32>( mJahAheadQueue.size() ), std::memory_order_relaxed );
                    return job;
                }
            }
        }
        return 0;
    }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::_jahPromote( const Hlms *owner, const uint32 finalHash )
    {
        if( !owner || owner->getType() == HLMS_COMPUTE ||
            mJahAheadQueued.load( std::memory_order_relaxed ) == 0u )
            return;
        {
            std::lock_guard<std::mutex> lock( mMutex );
            for( std::deque<HlmsAsyncJob *>::iterator it = mJahAheadQueue.begin(); it != mJahAheadQueue.end();
                 ++it )
            {
                // a graphics job's key IS its stub entry (Hlms::requestAsyncEntry): its hash is the permutation's
                if( ( *it )->mOwner == owner &&
                    static_cast<const HlmsCache *>( ( *it )->mKey )->hash == finalHash )
                {
                    mQueue.push_back( *it );
                    mJahAheadQueue.erase( it );
                    mJahAheadQueued.store( static_cast<uint32>( mJahAheadQueue.size() ),
                                           std::memory_order_relaxed );
                    mJahPromoted.fetch_add( 1u, std::memory_order_relaxed );
                    break;
                }
            }
        }
        mWorkCv.notify_one();
    }
    //-----------------------------------------------------------------------------------
    size_t HlmsAsyncCompiler::publish()
    {
        std::vector<HlmsAsyncJob *> done;
        {
            std::lock_guard<std::mutex> lock( mMutex );
            if( mFinished.empty() )
                return 0u;
            done.swap( mFinished );
        }
        for( HlmsAsyncJob *job : done )
            finish( job, true );
        return done.size();
    }
    //-----------------------------------------------------------------------------------
    bool HlmsAsyncCompiler::waitFor( const void *key, const char *caller )
    {
        std::unique_lock<std::mutex> lock( mMutex );

        // STILL QUEUED (the worker is late: every thread busy, or starved by load). The main
        // thread NEVER builds it itself while the service runs — that was a UI-thread compile
        // exactly when the service fell behind (shader.sample_opens_quiet, 2 of 7 gate runs
        // under load). It goes to the FRONT of the queue and the caller waits for a service
        // thread like any running job. Only with no service thread left (stopped under the
        // caller) does the main thread build it, on its own slot.
        // (Jahshaka) in either class's queue: a waited ahead job moves to the front of the frame's own queue
        if( HlmsAsyncJob *job = jahTakeQueued( key ) )
        {
            {
                if( mThreads.empty() )
                {
                    lock.unlock();
                    LogManager::getSingleton().logMessage(
                        String( "HlmsAsyncCompiler: MAIN-THREAD BUILD of a queued permutation (" ) +
                        caller + ", no service thread): " + job->mWhat );
                    runJob( job, Hlms::kAsyncTidBase + kMaxThreads );
                    mNumCompleted.fetch_add( 1u, std::memory_order_relaxed );
                    if( job->mFailed )
                        mNumFailed.fetch_add( 1u, std::memory_order_relaxed );
                    finish( job, true );
                    return true;
                }
                mQueue.push_front( job );
                mWorkCv.notify_one();
                LogManager::getSingleton().logMessage(
                    String( "HlmsAsyncCompiler: a queued permutation moved to the front (" ) + caller +
                    " waits for the service): " + job->mWhat );
                mDoneCv.wait( lock, [this, key] {
                    for( HlmsAsyncJob *q : mQueue )
                    {
                        if( q->mKey == key )
                            return mThreads.empty();
                    }
                    for( HlmsAsyncJob *r : mRunning )
                    {
                        if( r && r->mKey == key )
                            return false;
                    }
                    return true;
                } );
            }
        }

        bool running = false;
        for( HlmsAsyncJob *job : mRunning )
            running |= job && job->mKey == key;

        if( running )
        {
            mDoneCv.wait( lock, [this, key] {
                for( HlmsAsyncJob *job : mRunning )
                {
                    if( job && job->mKey == key )
                        return false;
                }
                return true;
            } );
        }

        for( std::vector<HlmsAsyncJob *>::iterator it = mFinished.begin(); it != mFinished.end(); ++it )
        {
            if( ( *it )->mKey == key )
            {
                HlmsAsyncJob *job = *it;
                mFinished.erase( it );
                lock.unlock();
                finish( job, true );
                return true;
            }
        }
        return false;
    }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::waitForAll()
    {
        while( getNumOutstanding() > 0u )
        {
            {
                std::unique_lock<std::mutex> lock( mMutex );
                mDoneCv.wait( lock, [this] { return !mFinished.empty() || mThreads.empty(); } );
                if( mThreads.empty() && mFinished.empty() )
                    break;
            }
            publish();
        }
    }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::cancel( Hlms *owner )
    {
        std::vector<HlmsAsyncJob *> dropped;
        {
            std::unique_lock<std::mutex> lock( mMutex );
            for( std::deque<HlmsAsyncJob *> *q : { &mQueue, &mJahAheadQueue } )
            {
                for( std::deque<HlmsAsyncJob *>::iterator it = q->begin(); it != q->end(); )
                {
                    if( ( *it )->mOwner == owner )
                    {
                        dropped.push_back( *it );
                        it = q->erase( it );
                    }
                    else
                        ++it;
                }
            }
            mJahAheadQueued.store( static_cast<uint32>( mJahAheadQueue.size() ), std::memory_order_relaxed );

            mDoneCv.wait( lock, [this, owner] {
                for( HlmsAsyncJob *job : mRunning )
                {
                    if( job && job->mOwner == owner )
                        return false;
                }
                return true;
            } );

            for( std::vector<HlmsAsyncJob *>::iterator it = mFinished.begin(); it != mFinished.end(); )
            {
                if( ( *it )->mOwner == owner )
                {
                    dropped.push_back( *it );
                    it = mFinished.erase( it );
                }
                else
                    ++it;
            }
        }
        for( HlmsAsyncJob *job : dropped )
            finish( job, false );
    }
    //-----------------------------------------------------------------------------------
    bool HlmsAsyncCompiler::cancelKey( const void *key )
    {
        std::vector<HlmsAsyncJob *> dropped;
        bool waited = false;
        {
            std::unique_lock<std::mutex> lock( mMutex );
            while( HlmsAsyncJob *job = jahTakeQueued( key ) )
                dropped.push_back( job );
            for( HlmsAsyncJob *job : mRunning )
                waited |= job && job->mKey == key;
            mDoneCv.wait( lock, [this, key] {
                for( HlmsAsyncJob *job : mRunning )
                {
                    if( job && job->mKey == key )
                        return false;
                }
                return true;
            } );
            for( std::vector<HlmsAsyncJob *>::iterator it = mFinished.begin(); it != mFinished.end(); )
            {
                if( ( *it )->mKey == key )
                {
                    dropped.push_back( *it );
                    it = mFinished.erase( it );
                }
                else
                    ++it;
            }
        }
        for( HlmsAsyncJob *job : dropped )
            finish( job, false );
        return waited;
    }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::cancelAll()
    {
        std::vector<HlmsAsyncJob *> dropped;
        {
            std::unique_lock<std::mutex> lock( mMutex );
            dropped.insert( dropped.end(), mQueue.begin(), mQueue.end() );
            dropped.insert( dropped.end(), mJahAheadQueue.begin(), mJahAheadQueue.end() );
            mQueue.clear();
            mJahAheadQueue.clear();
            mJahAheadQueued.store( 0u, std::memory_order_relaxed );
            mDoneCv.wait( lock, [this] {
                for( HlmsAsyncJob *job : mRunning )
                {
                    if( job )
                        return false;
                }
                return true;
            } );
            dropped.insert( dropped.end(), mFinished.begin(), mFinished.end() );
            mFinished.clear();
        }
        for( HlmsAsyncJob *job : dropped )
            finish( job, false );
    }
}  // namespace Ogre

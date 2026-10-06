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
        mThreadNice( 0 )
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
            mWorkCv.wait( lock, [this] { return mStop || !mQueue.empty(); } );
            if( mStop )
                break;

            HlmsAsyncJob *job = mQueue.front();
            mQueue.pop_front();
            mRunning[threadIdx] = job;
            lock.unlock();

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
            dropped.insert( dropped.end(), mFinished.begin(), mFinished.end() );
            mQueue.clear();
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
            if( urgent )
                mQueue.push_front( job );
            else
                mQueue.push_back( job );
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
    bool HlmsAsyncCompiler::waitFor( const void *key )
    {
        std::unique_lock<std::mutex> lock( mMutex );

        // Still queued: nobody has started it, so the main thread builds it itself on its own
        // slot rather than wait behind the queue.
        for( std::deque<HlmsAsyncJob *>::iterator it = mQueue.begin(); it != mQueue.end(); ++it )
        {
            if( ( *it )->mKey == key )
            {
                HlmsAsyncJob *job = *it;
                mQueue.erase( it );
                lock.unlock();
                runJob( job, Hlms::kAsyncTidBase + kMaxThreads );
                mNumCompleted.fetch_add( 1u, std::memory_order_relaxed );
                if( job->mFailed )
                    mNumFailed.fetch_add( 1u, std::memory_order_relaxed );
                finish( job, true );
                return true;
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
            for( std::deque<HlmsAsyncJob *>::iterator it = mQueue.begin(); it != mQueue.end(); )
            {
                if( ( *it )->mOwner == owner )
                {
                    dropped.push_back( *it );
                    it = mQueue.erase( it );
                }
                else
                    ++it;
            }

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
    void HlmsAsyncCompiler::cancelKey( const void *key )
    {
        std::vector<HlmsAsyncJob *> dropped;
        {
            std::unique_lock<std::mutex> lock( mMutex );
            for( std::deque<HlmsAsyncJob *>::iterator it = mQueue.begin(); it != mQueue.end(); )
            {
                if( ( *it )->mKey == key )
                {
                    dropped.push_back( *it );
                    it = mQueue.erase( it );
                }
                else
                    ++it;
            }
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
    }
    //-----------------------------------------------------------------------------------
    void HlmsAsyncCompiler::cancelAll()
    {
        std::vector<HlmsAsyncJob *> dropped;
        {
            std::unique_lock<std::mutex> lock( mMutex );
            dropped.insert( dropped.end(), mQueue.begin(), mQueue.end() );
            mQueue.clear();
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

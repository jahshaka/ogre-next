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
#ifndef _OgreHlmsAsyncCompiler_H_
#define _OgreHlmsAsyncCompiler_H_

#include "OgrePrerequisites.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    /** \addtogroup Core
     *  @{
     */
    /** \addtogroup Resources
     *  @{
     */

    /** One unit of work for the HlmsAsyncCompiler: a shader permutation (template parse,
        the shading-language compile, the pipeline) built OUTSIDE the frame.

        A job carries everything it needs BY VALUE. It never holds a Renderable, a
        MovableObject or anything else whose life the main thread decides between frames:
        whatever the main thread had to read from those was read when the job was made.
    @remarks
        run() is called on a service thread, exactly once, unless the job is cancelled first.
        Then exactly one of publish() or discard() is called ON THE MAIN THREAD. The
        compiler deletes the job afterwards.
    */
    class _OgreExport HlmsAsyncJob : public OgreAllocatedObj
    {
    public:
        /// The Hlms the job compiles for; HlmsAsyncCompiler::cancel( hlms ) drops its jobs.
        Hlms *mOwner;
        /// What the main thread waits on (HlmsAsyncCompiler::waitFor): the stub entry, the
        /// compute job. Unique per pending job.
        const void *mKey;
        /// Set by run() when the permutation could not be built (the log says why).
        bool mFailed;
        /// What the job builds, for the log (the Hlms, the hash, the datablock / the compute
        /// job's name). Set on the main thread when the job is made.
        String mWhat;

        HlmsAsyncJob( Hlms *owner, const void *key ) : mOwner( owner ), mKey( key ), mFailed( false )
        {
        }
        virtual ~HlmsAsyncJob();

        /// Service thread. @param tid The Hlms thread slot reserved for this thread
        /// (Hlms::kAsyncTidBase + n). Exceptions are caught by the caller and mark the job failed.
        virtual void run( size_t tid ) = 0;
        /// Main thread: the job ran; hand its result to its owner.
        virtual void publish() = 0;
        /// Main thread: the job is dropped (cancelled before or after it ran). Release what it holds.
        virtual void discard() = 0;
    };

    /** THE SHADER COMPILE SERVICE (Jahshaka fork, ASYNC-SHADERS-1).

        Ogre-Next compiles a missing permutation inside the frame that needs it: serially, or
        on the SceneManager's workers with the main thread parked in
        ParallelHlmsCompileQueue::stopAndWait until every request of the frame is done. Either
        way the frame waits for glslang and the driver. This service compiles OUTSIDE the
        frame on its own persistent threads; the main thread submits jobs and publishes the
        finished ones at the start of the next frame (Root::_updateAllRenderTargets), and an
        asynchronous pass draws a placeholder for an entry that has not landed yet (see
        Hlms::getMaterialAsync and CompositorWorkspace::setAsyncShaderCompile).

        Owned by the HlmsManager; off (zero threads) until setNumThreads.
    */
    class _OgreExport HlmsAsyncCompiler : public OgreAllocatedObj
    {
    public:
        /// The most service threads; each owns Hlms thread slot kAsyncTidBase + i. One more
        /// slot (kAsyncTidBase + kMaxThreads) is the MAIN thread's, used when it runs a queued
        /// job itself (waitFor).
        static constexpr uint32 kMaxThreads = 8u;

    protected:
        std::mutex              mMutex;
        std::condition_variable mWorkCv;  ///< service threads wait for work
        std::condition_variable mDoneCv;  ///< the main thread waits for a job to finish

        std::deque<HlmsAsyncJob *>  mQueue;     // GUARDED_BY( mMutex )
        std::vector<HlmsAsyncJob *> mRunning;   // GUARDED_BY( mMutex ), one per thread (or null)
        std::vector<HlmsAsyncJob *> mFinished;  // GUARDED_BY( mMutex )
        std::vector<std::thread>    mThreads;
        bool                        mStop;  // GUARDED_BY( mMutex )

        std::atomic<uint32> mNumOutstanding;  ///< queued + running + finished-unpublished
        std::atomic<uint64> mNumCompleted;    ///< jobs that ran to the end (success or failure)
        std::atomic<uint64> mNumFailed;
        int                 mThreadNice;
        bool                mPlaceholdersOnly;  ///< main thread only

        void threadMain( uint32 threadIdx );
        void stopThreads();
        /// Main thread. Publishes or discards one finished job and deletes it.
        void finish( HlmsAsyncJob *job, bool publishIt );

    public:
        HlmsAsyncCompiler();
        ~HlmsAsyncCompiler();

        /** Starts (or restarts with a new count) the service threads. 0 stops the service:
            everything pending is discarded. Main thread only.
        @param niceIncrement
            On Linux each service thread lowers its own scheduling priority by this much
            (setpriority on its tid) so a compile never competes with the frame's workers.
        */
        void   setNumThreads( uint32 numThreads, int niceIncrement = 10 );
        uint32 getNumThreads() const { return static_cast<uint32>( mThreads.size() ); }
        bool   isRunning() const { return !mThreads.empty(); }

        /// Main thread. Takes ownership. The service must be running. An URGENT job goes to the
        /// front of the queue (a placeholder: everything waiting draws with it).
        void submit( HlmsAsyncJob *job, bool urgent = false );

        /// Main thread. Publishes every finished job. Returns how many it published.
        size_t publish();

        /** Main thread. Blocks until the job with this key has run, then publishes it. A job
            still QUEUED is taken off the queue and run here, on the main thread's slot.
        @return
            False if no job with that key is pending.
        */
        bool waitFor( const void *key, const char *caller = "a blocking pass" );

        /// Main thread. Blocks until nothing is pending, publishing as jobs finish.
        void waitForAll();

        /** Main thread. Drops every job of this Hlms: queued ones are discarded unrun, running
            ones are waited for and discarded, finished ones are discarded. Call before the
            Hlms invalidates what its jobs point at (its shader cache, its templates, itself).
        */
        void cancel( Hlms *owner );

        /// Main thread. Drops every job (see cancel).
        void cancelAll();

        /// Main thread. Drops every job with this key (see cancel): what a job points at is
        /// about to die (a compute job being destroyed).
        void cancelKey( const void *key );

        /** Main thread. While set, Hlms::getMaterialAsync requests only PLACEHOLDERS: an object
            whose own permutation is not built requests the placeholder for the pass it is drawn
            in and leaves its own stub unrequested (COMPILATION_REQUIRED, requested by the next
            draw that finds it so). For a warm-up that must leave every pass's placeholders
            built without paying for every object's permutation in every pass.
        */
        void setPlaceholdersOnly( bool only ) { mPlaceholdersOnly = only; }
        bool getPlaceholdersOnly() const { return mPlaceholdersOnly; }

        /// Jobs submitted and not yet published or discarded.
        uint32 getNumOutstanding() const { return mNumOutstanding.load( std::memory_order_relaxed ); }
        uint64 getNumCompleted() const { return mNumCompleted.load( std::memory_order_relaxed ); }
        uint64 getNumFailed() const { return mNumFailed.load( std::memory_order_relaxed ); }

        /// True on a service thread only. A job the main thread runs itself (waitFor) is a
        /// compile the main thread waited for, and says so by answering false.
        static bool isServiceThread();
    };

    /** @} */
    /** @} */
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif

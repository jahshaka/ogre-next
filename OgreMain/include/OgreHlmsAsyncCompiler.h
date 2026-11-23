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
        /// (Jahshaka, SHADER-SYSTEM-1 stage 7) THE AHEAD CLASS: jobs submitted while _jahSetAheadClass is on
        /// (a pass as it WILL be: RenderQueue::AheadListener, the engine's request-only passes). A service thread
        /// takes one only when mQueue is empty: the frame's own requests are always served first.
        std::deque<HlmsAsyncJob *>  mJahAheadQueue;  // GUARDED_BY( mMutex )
        std::vector<HlmsAsyncJob *> mRunning;   // GUARDED_BY( mMutex ), one per thread (or null)
        std::vector<HlmsAsyncJob *> mFinished;  // GUARDED_BY( mMutex )
        std::vector<std::thread>    mThreads;
        bool                        mStop;  // GUARDED_BY( mMutex )

        std::atomic<uint32> mNumOutstanding;  ///< queued + running + finished-unpublished
        std::atomic<uint64> mNumCompleted;    ///< jobs that ran to the end (success or failure)
        std::atomic<uint64> mNumFailed;
        std::atomic<uint64> mJahNumResolved{ 0u };  ///< (Jahshaka) published or discarded (getJahNumResolved)
        int                 mThreadNice;
        bool                mPlaceholdersOnly;  ///< main thread only
        bool                mJahFailRequests = false;  ///< (Jahshaka) the test door _jahFailRequests
        std::atomic<uint32> mJahServiceDelayMs{ 0u };  ///< (Jahshaka) the test door _jahServiceDelayMs
        bool                mJahAheadClass = false;   ///< (Jahshaka) main thread: submit() into the ahead class
        std::atomic<uint64> mJahAheadRun{ 0u };       ///< (Jahshaka) ahead-class jobs a service thread took
        std::atomic<uint32> mJahAheadQueued{ 0u };    ///< (Jahshaka) mJahAheadQueue's size, read without the lock
        std::atomic<uint64> mJahPromoted{ 0u };       ///< (Jahshaka) ahead jobs a real request promoted
        /// (Jahshaka) Under mMutex: the queued job with this key taken out of either class's queue, or null.
        HlmsAsyncJob *jahTakeQueued( const void *key );

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
        /// about to die (a compute job being destroyed). (Jahshaka) True when a job with this
        /// key was RUNNING and the call waited for it to finish (HlmsCompute counts those waits).
        bool cancelKey( const void *key );

        /** Main thread. While set, Hlms::getMaterialAsync requests only PLACEHOLDERS: an object
            whose own permutation is not built requests the placeholder for the pass it is drawn
            in and leaves its own stub unrequested (COMPILATION_REQUIRED, requested by the next
            draw that finds it so). For a warm-up that must leave every pass's placeholders
            built without paying for every object's permutation in every pass.
        */
        void setPlaceholdersOnly( bool only ) { mPlaceholdersOnly = only; }
        bool getPlaceholdersOnly() const { return mPlaceholdersOnly; }

        /** (Jahshaka) A TEST DOOR, main thread, default off (COLD-BOOT-1 item 3: the engine's kArms row
            shaders.asyncFail drives it, gi.card_build_late's failure case): while on, every NON-URGENT request
            - an object's own permutation; a placeholder's request is urgent and is never failed - FAILS at its
            request (Hlms::requestAsyncEntry), the outcome of a request that cannot be made. It forces the one
            state a test cannot otherwise reach: a permutation that fails, which is FINAL (it draws its
            placeholder as its answer, never counted as pending: Hlms::getMaterialAsync, RenderQueue).
        */
        void _jahFailRequests( bool on ) { mJahFailRequests = on; }
        bool _jahFailRequestsOn() const { return mJahFailRequests; }
        /** (Jahshaka) A TEST DOOR (the arm shaders.serviceSlow): while above 0, every job a service thread takes is
            held that many milliseconds before it runs - a slow service (a loaded box, a cold driver) made on demand,
            so a frame's dependence on WHEN a background build lands can be shown (SHADER-SYSTEM-1 7a R2). Any thread.
        */
        void _jahServiceDelayMs( uint32 ms ) { mJahServiceDelayMs.store( ms, std::memory_order_relaxed ); }
        /** (Jahshaka, SHADER-SYSTEM-1 stage 7) THE AHEAD CLASS, main thread: while on, submit() queues into the
            ahead class - jobs for a pass as it WILL be after a named event (RenderQueue::AheadListener's ahead
            pass; the engine's request-only passes), which a service thread takes only when the frame's own queue
            is empty. The frame's own requests are always served first (the owner, 2026-11-23: faster loading over
            correctness while shaders stream in): an ahead request never delays an object the frame draws. An
            urgent ahead job goes to the front of the ahead class only. A waitFor of an ahead job moves it to the
            front of the frame's own queue (a pass waits for it now).
        */
        void _jahSetAheadClass( bool on ) { mJahAheadClass = on; }
        bool _jahAheadClassOn() const { return mJahAheadClass; }
        /** (Jahshaka) THE PROMOTION, main thread: a REAL request (a pass drawing now) whose permutation `finalHash` of
            `owner` (a graphics Hlms) is still queued in the ahead class - asked ahead for another view's pass as it
            will be, the same permutation this pass needs now - moves to the back of the frame's own queue: it is a
            request of this frame now. Free while the ahead class is empty (one atomic read).
        */
        void _jahPromote( const Hlms *owner, uint32 finalHash );
        uint64 getJahPromoted() const { return mJahPromoted.load( std::memory_order_relaxed ); }
        /// (Jahshaka) Ahead-class jobs a service thread has taken (any thread).
        uint64 getJahAheadRun() const { return mJahAheadRun.load( std::memory_order_relaxed ); }
        /// (Jahshaka) A request that failed before it became a job counts as a failure too (getNumFailed).
        void _jahCountRequestFailed() { mNumFailed.fetch_add( 1u, std::memory_order_relaxed ); }

        /// Jobs submitted and not yet published or discarded.
        uint32 getNumOutstanding() const { return mNumOutstanding.load( std::memory_order_relaxed ); }
        uint64 getNumCompleted() const { return mNumCompleted.load( std::memory_order_relaxed ); }
        uint64 getNumFailed() const { return mNumFailed.load( std::memory_order_relaxed ); }
        /// (Jahshaka) Jobs RESOLVED - published or discarded (a cancelled one included): each has left
        /// getNumOutstanding for good. getNumCompleted counts a job when it finishes RUNNING (a failure
        /// included), before the main thread publishes it, and never a job cancelled unrun - so
        /// completed + outstanding counts a run-but-unpublished job twice. Submitted = resolved +
        /// outstanding, exactly (the engine's "n of m": SHADER-SYSTEM-1 stage 7).
        uint64 getJahNumResolved() const { return mJahNumResolved.load( std::memory_order_relaxed ); }

        /// True on a service thread only. A job the main thread runs itself (waitFor) is a
        /// compile the main thread waited for, and says so by answering false.
        static bool isServiceThread();
    };

    /** @} */
    /** @} */
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif

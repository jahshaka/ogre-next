/*
-----------------------------------------------------------------------------
This source file is part of OGRE-Next
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2014 Torus Knot Software Ltd

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
#ifndef _OgreHlmsCompute_H_
#define _OgreHlmsCompute_H_

#include "OgreHlms.h"
#include <memory>
#include <set>

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class CompositorShadowNode;
    struct QueuedRenderable;

    /** \addtogroup Component
     *  @{
     */
    /** \addtogroup Material
     *  @{
     */

    /** HLMS implementation that handles compute shaders. It isn't registered in
        the usual way to the HlmsManager.
    */
    class _OgreExport HlmsCompute : public Hlms
    {
        struct ComputeJobEntry
        {
            HlmsComputeJob *computeJob;
            String          name;
            ComputeJobEntry() : computeJob( 0 ) {}
            ComputeJobEntry( HlmsComputeJob *_computeJob, const String &_name ) :
                computeJob( _computeJob ),
                name( _name )
            {
            }
        };

        typedef std::map<IdString, ComputeJobEntry> HlmsComputeJobMap;

        struct Hash
        {
            uint64 hashVal[2];

            bool operator<( const Hash &_r ) const
            {
                if( hashVal[0] < _r.hashVal[0] )
                    return true;
                if( hashVal[0] > _r.hashVal[0] )
                    return false;

                if( hashVal[1] < _r.hashVal[1] )
                    return true;
                // if( hashVal[1] > _r.hashVal[1] ) return false;

                return false;
            }
        };

        struct ComputePsoCache
        {
            HlmsComputeJob *job;
            HlmsPropertyVec setProperties;
            HlmsComputePso  pso;
            uint32          paramsUpdateCounter;
            uint32          paramsProfileUpdateCounter;
            /// (Jahshaka) the parameters were checked to fit the program (dispatch()'s first dispatch)
            bool            jahParamsFit = false;
            /// (Jahshaka) the job's parameters are OWED to this program: an asynchronously published
            /// PSO gets them whole at its own first dispatch (asyncComputePublished), as the in-frame
            /// build gets them at the dispatch that builds it (compileShader)
            bool            jahParamsOwed = false;

            ComputePsoCache() : job( 0 ), paramsUpdateCounter( ~0u ), paramsProfileUpdateCounter( ~0u )
            {
            }
            ComputePsoCache( HlmsComputeJob *_job, const HlmsPropertyVec &properties ) :
                job( _job ),
                setProperties( properties ),
                paramsUpdateCounter( ~0u ),
                paramsProfileUpdateCounter( ~0u )
            {
            }

            bool operator==( const ComputePsoCache &_r ) const
            {
                // Exclude the PSO from the comparison!
                return setProperties == _r.setProperties && job == _r.job;
            }
        };

        typedef vector<ComputePsoCache>::type  ComputePsoCacheVec;
        typedef map<Hash, GpuProgramPtr>::type CompiledShaderMap;

    public:
        /** (Jahshaka fork, THE COMPUTE CODE RECORDS: COLD-BOOT-1 stage 3, SHADER-SYSTEM-1 stage 3F.) What a built
            permutation's code needs to come back WITHOUT its template parse: the generated source, the root layout it
            was hashed and built with, and the parse's outputs a PSO needs. Filed under the INPUT KEY (msJahComputeKeyHook:
            the content the parse reads) at both build sites - compileShader and an async publish - and by
            jahPrewarmCode; a host persists them (getJahRecords) and hands them back at the next launch. */
        struct JahFileHash
        {
            String name;      ///< a file the parse read (the template, a piece file), as opened
            uint64 hash[2];   ///< MurmurHash3_x64_128 of its bytes, IdString::Seed
        };
        struct JahCodeRecord
        {
            uint64 inputKey[2];
            uint64 programKey[2];  ///< mCompiledShaderCache's key: jahProgramKey( source, rootLayout, explicitApi )
            String name;           ///< the job's name: logs only, never a key
            String sourceFilename; ///< the template's file name (with its extension)
            String source;         ///< the GENERATED source
            String rootLayout;     ///< the RootLayout's bytes (memset + HlmsComputeJob::setupRootLayout)
            bool   explicitApi;    ///< RSC_EXPLICIT_API: the root layout is part of the program key
            bool   arrayBindings;  ///< uses_array_bindings: the program reflects its array bindings
            bool   indirect;
            bool   jobSized;
            uint32 threadsPerGroup[3];
            uint32 numThreadGroups[3];
            vector<JahFileHash>::type files;
            JahCodeRecord();
        };
        typedef std::shared_ptr<const JahCodeRecord> JahCodeRecordPtr;
        /// What a compute template's parse READS, handed to msJahComputeKeyHook: the template's bytes, every piece
        /// file's (name, bytes) in the job's order, the job's C++ pieces, the properties AFTER the render system's
        /// set-up (what the parse starts from) and the root layout's bytes (two jobs of equal text whose slots differ
        /// in type need two programs: the start preflight's F2).
        struct JahKeyInputs
        {
            const String                                            *templateText;
            const vector<std::pair<String, const String *> >::type *pieceFiles;
            const PiecesMap                                         *cppPieces;
            const HlmsPropertyVec                                   *properties;
            const String                                            *rootLayout;
        };
        /** THE KEY HOOK (upstream's custom-hook pattern, as msJahDispatchHook): the host's content key of the inputs.
            Null (the default): NO RECORDS - compileShader parses every permutation, exactly as upstream; nothing is
            filed, nothing hits. */
        static void ( *msJahComputeKeyHook )( const JahKeyInputs &in, uint64 outKey[2] );

    private:
        typedef std::map<Hash, JahCodeRecordPtr> JahRecordMap;
        /// Main thread only (every build site files there; the host snapshots there).
        JahRecordMap mJahRecords;
        uint64       mJahRecordGeneration = 0u;  ///< bumps on every record filed or changed
        /// (Jahshaka, SHADER-SYSTEM-1 F10) the input keys of the records USED this launch: a hit, or a build's filing
        /// (main thread: every caller of jahHit and jahFileRecord is; a prewarm files without using).
        std::set<Hash> mJahUsedRecords;
        /// (Jahshaka, SHADER-SYSTEM-1) THE CODE JOBS IN FLIGHT, by their record's input key -> the job's key in the
        /// HlmsAsyncCompiler (main thread: filled at jahPrewarmCode's submit, erased at the job's publish or discard).
        /// A hit on such a record JOINS its job (jahHit) instead of parsing past it.
        std::map<Hash, const void *> mJahCodeJobs;
        /// The bytes of every file a parse or a key read, by file name (main thread): read once from the resource
        /// system. Cleared by clearShaderCache (reloadFrom calls it).
        std::map<String, String> mJahFileMemo;
        /// (Jahshaka) THE IN-FRAME COUNTERS: a template parse run on the main thread (compileShader, or a queued
        /// request the main thread ran itself), and a compute PSO built in compileShader (any path: a parse, a record's
        /// hit, a clone). Main thread.
        uint64 mJahParsedInFrame = 0u;
        uint64 mJahPsoInFrame = 0u;
        /// (Jahshaka) destroyComputeJob calls that WAITED for the job's running compile (HlmsAsyncCompiler::cancelKey),
        /// and the time they waited: a stall of the render thread at a clone's teardown (the start preflight's F4).
        uint64 mJahDestroyWaits = 0u;
        uint64 mJahDestroyWaitUs = 0u;

        class AsyncCodeJob;
        friend class AsyncCodeJob;
        /// What a parse reads, read once: the template's name and bytes, the piece files, the root layout's bytes.
        struct JahParseInputs
        {
            String                                           sourceFilename;
            const String                                    *templateText;
            vector<std::pair<String, const String *> >::type pieceFiles;
            String                                           rootLayout;
        };
        /// The render system's properties every parse starts from (mT[tid]: GL3Plus, the syntaxes, the precision...).
        void jahSetRsProperties( size_t tid );
        /// processPieces' extension rule: the file name a piece file is opened under.
        String jahPieceFileName( const String &file ) const;
        /// A file's bytes, through the memo.
        const String &jahFileText( const String &filename );
        void jahGatherInputs( HlmsComputeJob *job, JahParseInputs &out );
        /// The input key of `in` under `properties` (mT[tid]'s, after jahSetRsProperties). False with no hook.
        bool jahInputKey( const JahParseInputs &in, const PiecesMap &cppPieces, const HlmsPropertyVec &properties,
                          Hash &outKey ) const;
        /// THE HIT: the record filed under `inputKey`, built with this root layout on this kind of API, whose program
        /// is compiled; null = a miss. `join`: a record whose code job is in flight waits for that job (the frame's
        /// synchronous build: compileShader) instead of missing; a deferred dispatch's question never waits.
        const JahCodeRecord *jahHit( const Hash &inputKey, const String &rootLayout, GpuProgramPtr &outProgram,
                                     bool join );
        /// Files a record (main thread); the generation moves only when the map changes.
        void jahFileRecord( const JahCodeRecordPtr &record );

    public:
        /// mCompiledShaderCache's key of a generated source: its hash, and on an explicit API that hash paired with
        /// the root layout's (compileShader's own steps).
        static void jahProgramKey( const String &source, const String &rootLayout, bool explicitApi,
                                   uint64 outKey[2] );
        /// Bumps whenever a record is filed or changed (a host compares it to know its saved copy is stale).
        uint64 getJahRecordGeneration() const { return mJahRecordGeneration; }
        /// Every record, in the map's (stable) order. Main thread.
        void getJahRecords( vector<JahCodeRecordPtr>::type &out ) const;
        /// (Jahshaka, F10) The input keys of the records used this launch (hit or built), for the host's prune.
        void getJahUsedRecordKeys( vector<std::pair<uint64, uint64> >::type &out ) const;
        /** The records a launch read: FILED (the hit sees them at once), and each one's code compiled on the
            HlmsAsyncCompiler - code only: no job, no PSO (its program lands in mCompiledShaderCache at the publish).
            A record already filed under its key is skipped; with no service running the records are filed and their
            code compiles at the first parse that needs it. Main thread. */
        void jahPrewarmCode( vector<JahCodeRecord>::type &records );
        uint64 getJahParsedInFrame() const { return mJahParsedInFrame; }
        uint64 getJahPsoInFrame() const { return mJahPsoInFrame; }
        uint64 getJahDestroyWaits() const { return mJahDestroyWaits; }
        uint64 getJahDestroyWaitUs() const { return mJahDestroyWaitUs; }

    private:

        AutoParamDataSource *mAutoParamDataSource;
        String const        *mComputeShaderTarget;

        /// Caches a compiled shader based on the hash of its source string
        /// We need this in case two HlmsComputeJobs use the same exact
        /// shader but with different buffers.
        CompiledShaderMap mCompiledShaderCache;
        /// Caches a full PSO.
        ComputePsoCacheVec mComputeShaderCache;
        FastArray<size_t>  mFreeShaderCacheEntries;

        HlmsComputeJobMap mComputeJobs;

        HlmsComputePso compileShader( HlmsComputeJob *job, uint32 finalHash );

        // ---- ASYNC-SHADERS-1 (Jahshaka fork): compute permutations built off the frame ----
        class AsyncComputeJob;
        friend class AsyncComputeJob;
        /// A permutation the HlmsAsyncCompiler is building, or failed to build. Main thread.
        struct AsyncComputeEntry
        {
            HlmsComputeJob  *job;
            HlmsPropertyVec  setProperties;
            AsyncComputeJob *inFlight;  ///< null once it failed
        };
        typedef vector<AsyncComputeEntry>::type AsyncComputeEntryVec;
        AsyncComputeEntryVec mAsyncComputeEntries;
        static bool msInAsyncWorkspace;
        bool   mDeferredDispatch;      ///< setDeferredDispatch
        uint64 mNumDeferredDispatches;
        /// (Jahshaka) dispatches refused because a PSO's parameters did not fit its program
        uint64 mJahParamsMisfits = 0u;
        /// (Jahshaka) the misfit counter's NEGATIVE CONTROL: the next N dispatches are refused as misfits
        uint32 mJahForceMisfits = 0u;
        uint32               mAsyncProgramCounter;

        /// Index into mAsyncComputeEntries, or size() when none.
        size_t findAsyncComputeEntry( const HlmsComputeJob *job, const HlmsPropertyVec &props ) const;
        /// The service thread's half: the template parse from the job's TEXT snapshot, the
        /// compile, the pipeline. The same steps compileShader takes, on slot `tid`.
        void compileAsync( AsyncComputeJob &request, size_t tid );
        /// Main thread: the request ran (published) or is dropped.
        void asyncComputePublished( AsyncComputeJob &request );
        void asyncComputeDiscarded( AsyncComputeJob &request );
        /// Parses piece files from their text (the async path reads them on the main thread).
        void processPieceTexts( const StringVector &pieceTexts, size_t tid );
        /// dispatch(): waits for an in-flight request of the job's current permutation.
        /// True if it waited (the permutation is published, or failed).
        bool waitForAsyncPermutation( HlmsComputeJob *job );

        HlmsDatablock *createDatablockImpl( IdString              datablockName,  //
                                            const HlmsMacroblock *macroblock,     //
                                            const HlmsBlendblock *blendblock,     //
                                            const HlmsParamVec   &paramVec ) override;

        void setupRootLayout( RootLayout &rootLayout, size_t tid ) override;

    public:
        HlmsCompute( AutoParamDataSource *autoParamDataSource );
        ~HlmsCompute() override;

        void reloadFrom( Archive *newDataFolder, ArchiveVec *libraryFolders = 0 ) override;

        /** An HlmsComputeJob is very similar to an HlmsDatablock, except it
            contains a compute job instead. If multiple HlmsComputeJob end up
            having the same compute shader (i.e. the resulting source code is
            the same); they will share the same shader.
        @param datablockName
            Name to assign to the job, for lookup.
        @param refName
            User-friendly readable name of the job. Normally should match the datablockName
        @param sourceFilename
            Main file to use for compiling.
        @param includedPieceFiles
            Included files, to be parsed to defined pieces for the main file to use (can be empty).
        @return
            A new job.
        */
        HlmsComputeJob *createComputeJob( IdString datablockName, const String &refName,
                                          const String       &sourceFilename,
                                          const StringVector &includedPieceFiles );

        /// Finds an existing Compute Job. If none found, throws an exception.
        HlmsComputeJob *findComputeJob( IdString datablockName ) const;

        /// Finds an existing Compute Job. If none found, returns null.
        HlmsComputeJob *findComputeJobNoThrow( IdString datablockName ) const;

        /// Returns the string name associated with its hashed name (this was
        /// passed as refName in createComputeJob()). Returns null ptr if
        /// not found.
        /// The reason this String doesn't live in HlmsComputeJob is to prevent
        /// cache trashing (jobs are hot iterated every frame, and the
        /// full name is rarely ever used)
        const String *getJobNameStr( IdString name ) const;

        /// Destroys a specific Compute Job. You are responsible for ensuring
        /// is not in use anywhere (otherwise a dangling pointer will ensue)
        void destroyComputeJob( IdString name );

        /// Destroys all jobs created via @see createComputeJob
        void destroyAllComputeJobs();

        /// Destroys the shader cache from all jobs, causing us to reload shaders from file again
        void clearShaderCache() override;

        /// Main function for dispatching a compute job.
        void dispatch( HlmsComputeJob *job, SceneManager *sceneManager, Camera *camera );

        /// Jahshaka (THE GPU CHECKPOINTS, PHOTON-7 stage 5): called with the job right before its dispatch is
        /// recorded (its PSO, descriptors and parameters bound; the indirect and the direct route alike), so a
        /// host can mark the command stream with the job's name (VK_NV_device_diagnostic_checkpoints) and, after a
        /// device loss, name the dispatch the GPU never finished. Null (the default): nothing is called.
        static void ( *msJahDispatchHook )( const HlmsComputeJob *job );

        enum AsyncReadiness
        {
            AsyncReady,    ///< the permutation is built: dispatch() will not compile
            AsyncPending,  ///< the HlmsAsyncCompiler is building it
            AsyncFailed    ///< it could not be built (the log says why); dispatch() would throw
        };

        /** (Jahshaka fork, ASYNC-SHADERS-1) Is the permutation the job's CURRENT properties
            select built? If not, it is handed to the HlmsManager's HlmsAsyncCompiler (once)
            and the answer is AsyncPending — nothing waits for it. A caller that must not
            stall (a GI rebuild after a project is open) asks this for every permutation it
            is about to dispatch and keeps its previous result until all answer AsyncReady;
            dispatch() itself stays blocking (and waits for an in-flight request of the same
            permutation instead of compiling it twice). With no service running the answer is
            AsyncReady: dispatch() compiles inside the frame, as it always did.
        */
        AsyncReadiness requestAsync( HlmsComputeJob *job );

        /** (ASYNC-SHADERS-1) DEFERRED DISPATCH. While set (main thread, scoped by the caller) — and
            for every dispatch a pass of an ASYNCHRONOUS workspace makes (its SceneManager's render
            queue says so for the length of the workspace's update) — a
            dispatch() whose permutation is not built never compiles it in the frame and never
            waits for it: the permutation goes to the background compiler (requestAsync) and the
            dispatch is SKIPPED, counted in getNumDeferredDispatches. A caller that sets it must
            treat a moved count as "this work did not happen" and redo it later (the scene's IBL
            convolution keeps its previous result and retries the next frame).
        */
        void setDeferredDispatch( bool on ) { mDeferredDispatch = on; }
        /// Main thread: CompositorWorkspace::_update says an ASYNCHRONOUS workspace is updating,
        /// so a dispatch made from inside it with no SceneManager (a pass listener's own compute,
        /// a GPU cull) defers like the workspace's passes do.
        static void _setInAsyncWorkspace( bool on ) { msInAsyncWorkspace = on; }
        static bool _getInAsyncWorkspace() { return msInAsyncWorkspace; }
        bool getDeferredDispatch() const { return mDeferredDispatch; }
        uint64 getNumDeferredDispatches() const { return mNumDeferredDispatches; }
        /// (Jahshaka) dispatches refused, their PSO's parameters not fitting its program (dispatch()):
        /// 0 on a sound build - a test asserts it.
        uint64 getJahParamsMisfits() const { return mJahParamsMisfits; }
        /// (Jahshaka) A TEST DOOR, the counter's negative control: the next `n` dispatches are refused and
        /// counted exactly as a misfit is (a row asserting the count 0 reads it non-zero under the door).
        void _jahForceParamsMisfits( uint32 n ) { mJahForceMisfits = n; }
        /// Permutations the background compiler is building for this Hlms (requestAsync).
        size_t getNumAsyncInFlight() const;

        void _changeRenderSystem( RenderSystem *newRs ) override;

        HlmsDatablock *createDefaultDatablock() override;

        uint32 fillBuffersFor( const HlmsCache *cache, const QueuedRenderable &queuedRenderable,
                               bool casterPass, uint32 lastCacheHash, uint32 lastTextureHash ) override;

        uint32 fillBuffersForV1( const HlmsCache *cache, const QueuedRenderable &queuedRenderable,
                                 bool casterPass, uint32 lastCacheHash,
                                 CommandBuffer *commandBuffer ) override;

        uint32 fillBuffersForV2( const HlmsCache *cache, const QueuedRenderable &queuedRenderable,
                                 bool casterPass, uint32 lastCacheHash,
                                 CommandBuffer *commandBuffer ) override;
    };

    struct _OgreExport ComputeProperty
    {
        static const IdString ThreadsPerGroupX;
        static const IdString ThreadsPerGroupY;
        static const IdString ThreadsPerGroupZ;
        static const IdString NumThreadGroupsX;
        static const IdString NumThreadGroupsY;
        static const IdString NumThreadGroupsZ;

        static const IdString TypedUavLoad;

        static const IdString NumTextureSlots;
        static const IdString MaxTextureSlot;
        static const char    *Texture;

        static const IdString NumUavSlots;
        static const IdString MaxUavSlot;
        static const char    *Uav;
    };

    /** @} */
    /** @} */

}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif

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

#ifndef __Id_H__
#define __Id_H__

// Jahshaka patch 0015: <atomic>, for the id counter below.
#include <atomic>

namespace Ogre
{
    /// Big projects with lots, lots of units for very long periods of time (MMORPGs?)
    /// may want to define this to 64-bit
    typedef Ogre::uint32 IdType;

    /**
        Usage:
        OGRE_NEW SceneNode( Id::generateNewId< Node >() )
    */
    class _OgreExport Id
    {
    public:
        // Jahshaka patch 0015: THE COUNTER IS ATOMIC.
        //
        // Upstream's comment used to read "This function assumes creation of
        // new objects can't be made from multiple threads!!!" and the counter
        // was a plain `IdType`. Jahshaka breaks that assumption on purpose: an
        // iris::SceneNode IS an Ogre::SceneNode (SPECS/SCENEGRAPH_SPEC.md), and
        // the asset-import worker builds its fragment off the main thread while
        // the main thread keeps rendering — and creating engine objects.
        //
        // Everything ELSE those two threads touch is already disjoint (they
        // create into different SceneManagers, so different node vectors and
        // different memory managers; the document side serialises its own
        // creations on iris::graph's mutex). This static was the one piece of
        // shared mutable state on both paths, and a lost increment hands two
        // live objects the same id — which in Jahshaka also aliases two
        // document handles onto one entry in the owner table.
        //
        // Relaxed ordering: the only requirement is uniqueness. Nothing orders
        // anything by id, and an uncontended relaxed fetch_add costs the same
        // as the increment it replaces.
        template <typename T>
        static IdType generateNewId()
        {
            static std::atomic<IdType> g_currentId{ 0 };
            return g_currentId.fetch_add( 1, std::memory_order_relaxed );
        }
    };

    class _OgreExport IdObject
    {
    private:
        friend struct IdCmp;  // Avoid calling getId()
        IdType mId;

    protected:
        /**In the rare case our derived class wants to override our Id
            (normally we don't want that, that's why it's private).
        */
        void _setId( IdType newId ) { mId = newId; }

    public:
        /** We don't call generateNewId() here, to prevent objects in the stack (i.e. local variables)
            that don't need an Id from incrementing the count; which is very dangerous if the caller
            is creating local objects from multiple threads (which should stay safe!).
            Instead our creator should do that.
        */
        IdObject( IdType id ) : mId( id ) {}

        /// Get the unique id of this object
        IdType getId() const { return mId; }

        bool operator()( const IdObject *left, const IdObject *right ) { return left->mId < right->mId; }

        bool operator()( const IdObject &left, const IdObject &right ) { return left.mId < right.mId; }
    };
}  // namespace Ogre

#endif

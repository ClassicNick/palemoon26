/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript API. */

#ifndef jsapi_h
#define jsapi_h

#include "mozilla/FloatingPoint.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/StandardInteger.h"
#include "mozilla/ThreadLocal.h"
#include "mozilla/TypeTraits.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "js-config.h"
#include "jsalloc.h"
#include "jspubtd.h"

#include "js/CallArgs.h"
#include "js/CharacterEncoding.h"
#include "js/HashTable.h"
#include "js/RootingAPI.h"
#include "js/Utility.h"
#include "js/Value.h"
#include "js/Vector.h"

/************************************************************************/

namespace JS {

typedef mozilla::RangedPtr<const jschar> CharPtr;

class StableCharPtr : public CharPtr {
  public:
    StableCharPtr(const StableCharPtr &s) : CharPtr(s) {}
    StableCharPtr(const mozilla::RangedPtr<const jschar> &s) : CharPtr(s) {}
    StableCharPtr(const jschar *s, size_t len) : CharPtr(s, len) {}
    StableCharPtr(const jschar *pos, const jschar *start, size_t len)
      : CharPtr(pos, start, len)
    {}
};

#if defined JS_THREADSAFE && defined DEBUG

class JS_PUBLIC_API(AutoCheckRequestDepth)
{
    JSContext *cx;
  public:
    AutoCheckRequestDepth(JSContext *cx);
    ~AutoCheckRequestDepth();
};

# define CHECK_REQUEST(cx) \
    JS::AutoCheckRequestDepth _autoCheckRequestDepth(cx)

#else

# define CHECK_REQUEST(cx) \
    ((void) 0)

#endif /* JS_THREADSAFE && DEBUG */

#ifdef DEBUG
/*
 * Assert that we're not doing GC on cx, that we're in a request as
 * needed, and that the compartments for cx and v are correct.
 * Also check that GC would be safe at this point.
 */
JS_PUBLIC_API(void)
AssertArgumentsAreSane(JSContext *cx, const Value &v);
#else
inline void AssertArgumentsAreSane(JSContext *cx, const Value &v) {
    /* Do nothing */
}
#endif /* DEBUG */

class JS_PUBLIC_API(AutoGCRooter) {
  public:
    AutoGCRooter(JSContext *cx, ptrdiff_t tag);

    ~AutoGCRooter() {
        JS_ASSERT(this == *stackTop);
        *stackTop = down;
    }

    /* Implemented in gc/RootMarking.cpp. */
    inline void trace(JSTracer *trc);
    static void traceAll(JSTracer *trc);
    static void traceAllWrappers(JSTracer *trc);

  protected:
    AutoGCRooter * const down;

    /*
     * Discriminates actual subclass of this being used.  If non-negative, the
     * subclass roots an array of values of the length stored in this field.
     * If negative, meaning is indicated by the corresponding value in the enum
     * below.  Any other negative value indicates some deeper problem such as
     * memory corruption.
     */
    ptrdiff_t tag_;

    enum {
        VALARRAY =     -2, /* js::AutoValueArray */
        PARSER =       -3, /* js::frontend::Parser */
        SHAPEVECTOR =  -4, /* js::AutoShapeVector */
        IDARRAY =      -6, /* js::AutoIdArray */
        DESCRIPTORS =  -7, /* js::AutoPropDescArrayRooter */
        ID =           -9, /* js::AutoIdRooter */
        VALVECTOR =   -10, /* js::AutoValueVector */
        DESCRIPTOR =  -11, /* js::AutoPropertyDescriptorRooter */
        STRING =      -12, /* js::AutoStringRooter */
        IDVECTOR =    -13, /* js::AutoIdVector */
        OBJVECTOR =   -14, /* js::AutoObjectVector */
        STRINGVECTOR =-15, /* js::AutoStringVector */
        SCRIPTVECTOR =-16, /* js::AutoScriptVector */
        NAMEVECTOR =  -17, /* js::AutoNameVector */
        HASHABLEVALUE=-18, /* js::HashableValue */
        IONMASM =     -19, /* js::jit::MacroAssembler */
        IONALLOC =    -20, /* js::jit::AutoTempAllocatorRooter */
        WRAPVECTOR =  -21, /* js::AutoWrapperVector */
        WRAPPER =     -22, /* js::AutoWrapperRooter */
        OBJOBJHASHMAP=-23, /* js::AutoObjectObjectHashMap */
        OBJU32HASHMAP=-24, /* js::AutoObjectUnsigned32HashMap */
        OBJHASHSET =  -25, /* js::AutoObjectHashSet */
        JSONPARSER =  -26, /* js::JSONParser */
        CUSTOM =      -27, /* js::CustomAutoRooter */
        FUNVECTOR =   -28  /* js::AutoFunctionVector */
    };

  private:
    AutoGCRooter ** const stackTop;

    /* No copy or assignment semantics. */
    AutoGCRooter(AutoGCRooter &ida) MOZ_DELETE;
    void operator=(AutoGCRooter &ida) MOZ_DELETE;
};

class AutoStringRooter : private AutoGCRooter {
  public:
    AutoStringRooter(JSContext *cx, JSString *str = NULL
                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, STRING), str_(str)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    void setString(JSString *str) {
        str_ = str;
    }

    JSString * string() const {
        return str_;
    }

    JSString ** addr() {
        return &str_;
    }

    JSString * const * addr() const {
        return &str_;
    }

    friend void AutoGCRooter::trace(JSTracer *trc);

  private:
    JSString *str_;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoArrayRooter : private AutoGCRooter {
  public:
    AutoArrayRooter(JSContext *cx, size_t len, Value *vec
                    MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, len), array(vec), skip(cx, array, len)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        JS_ASSERT(tag_ >= 0);
    }

    void changeLength(size_t newLength) {
        tag_ = ptrdiff_t(newLength);
        JS_ASSERT(tag_ >= 0);
    }

    void changeArray(Value *newArray, size_t newLength) {
        changeLength(newLength);
        array = newArray;
    }

    Value *array;

    MutableHandleValue handleAt(size_t i)
    {
        JS_ASSERT(i < size_t(tag_));
        return MutableHandleValue::fromMarkedLocation(&array[i]);
    }
    HandleValue handleAt(size_t i) const
    {
        JS_ASSERT(i < size_t(tag_));
        return HandleValue::fromMarkedLocation(&array[i]);
    }

    friend void AutoGCRooter::trace(JSTracer *trc);

  private:
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    js::SkipRoot skip;
};

template<class T>
class AutoVectorRooter : protected AutoGCRooter
{
  public:
    explicit AutoVectorRooter(JSContext *cx, ptrdiff_t tag
                              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, tag), vector(cx), vectorRoot(cx, &vector)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    typedef T ElementType;

    size_t length() const { return vector.length(); }
    bool empty() const { return vector.empty(); }

    bool append(const T &v) { return vector.append(v); }
    bool append(const AutoVectorRooter<T> &other) {
        return vector.append(other.vector);
    }

    bool insert(T *p, const T &val) { return vector.insert(p, val); }

    /* For use when space has already been reserved. */
    void infallibleAppend(const T &v) { vector.infallibleAppend(v); }

    void popBack() { vector.popBack(); }
    T popCopy() { return vector.popCopy(); }

    bool growBy(size_t inc) {
        size_t oldLength = vector.length();
        if (!vector.growByUninitialized(inc))
            return false;
        makeRangeGCSafe(oldLength);
        return true;
    }

    bool resize(size_t newLength) {
        size_t oldLength = vector.length();
        if (newLength <= oldLength) {
            vector.shrinkBy(oldLength - newLength);
            return true;
        }
        if (!vector.growByUninitialized(newLength - oldLength))
            return false;
        makeRangeGCSafe(oldLength);
        return true;
    }

    void clear() { vector.clear(); }

    bool reserve(size_t newLength) {
        return vector.reserve(newLength);
    }

    T &operator[](size_t i) { return vector[i]; }
    const T &operator[](size_t i) const { return vector[i]; }

    JS::MutableHandle<T> handleAt(size_t i) { return JS::MutableHandle<T>::fromMarkedLocation(&vector[i]); }
    JS::Handle<T> handleAt(size_t i) const { return JS::Handle<T>::fromMarkedLocation(&vector[i]); }

    const T *begin() const { return vector.begin(); }
    T *begin() { return vector.begin(); }

    const T *end() const { return vector.end(); }
    T *end() { return vector.end(); }

    const T &back() const { return vector.back(); }

    friend void AutoGCRooter::trace(JSTracer *trc);

  private:
    void makeRangeGCSafe(size_t oldLength) {
        T *t = vector.begin() + oldLength;
        for (size_t i = oldLength; i < vector.length(); ++i, ++t)
            memset(t, 0, sizeof(T));
    }

    typedef js::Vector<T, 8> VectorImpl;
    VectorImpl vector;

    /* Prevent overwriting of inline elements in vector. */
    js::SkipRoot vectorRoot;

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

template<class Key, class Value>
class AutoHashMapRooter : protected AutoGCRooter
{
  private:
    typedef js::HashMap<Key, Value> HashMapImpl;

  public:
    explicit AutoHashMapRooter(JSContext *cx, ptrdiff_t tag
                               MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, tag), map(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    typedef Key KeyType;
    typedef Value ValueType;
    typedef typename HashMapImpl::Lookup Lookup;
    typedef typename HashMapImpl::Ptr Ptr;
    typedef typename HashMapImpl::AddPtr AddPtr;

    bool init(uint32_t len = 16) {
        return map.init(len);
    }
    bool initialized() const {
        return map.initialized();
    }
    Ptr lookup(const Lookup &l) const {
        return map.lookup(l);
    }
    void remove(Ptr p) {
        map.remove(p);
    }
    AddPtr lookupForAdd(const Lookup &l) const {
        return map.lookupForAdd(l);
    }

    template<typename KeyInput, typename ValueInput>
    bool add(AddPtr &p, const KeyInput &k, const ValueInput &v) {
        return map.add(p, k, v);
    }

    bool add(AddPtr &p, const Key &k) {
        return map.add(p, k);
    }

    template<typename KeyInput, typename ValueInput>
    bool relookupOrAdd(AddPtr &p, const KeyInput &k, const ValueInput &v) {
        return map.relookupOrAdd(p, k, v);
    }

    typedef typename HashMapImpl::Range Range;
    Range all() const {
        return map.all();
    }

    typedef typename HashMapImpl::Enum Enum;

    void clear() {
        map.clear();
    }

    void finish() {
        map.finish();
    }

    bool empty() const {
        return map.empty();
    }

    uint32_t count() const {
        return map.count();
    }

    size_t capacity() const {
        return map.capacity();
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return map.sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return map.sizeOfIncludingThis(mallocSizeOf);
    }

    unsigned generation() const {
        return map.generation();
    }

    /************************************************** Shorthand operations */

    bool has(const Lookup &l) const {
        return map.has(l);
    }

    template<typename KeyInput, typename ValueInput>
    bool put(const KeyInput &k, const ValueInput &v) {
        return map.put(k, v);
    }

    template<typename KeyInput, typename ValueInput>
    bool putNew(const KeyInput &k, const ValueInput &v) {
        return map.putNew(k, v);
    }

    Ptr lookupWithDefault(const Key &k, const Value &defaultValue) {
        return map.lookupWithDefault(k, defaultValue);
    }

    void remove(const Lookup &l) {
        map.remove(l);
    }

    friend void AutoGCRooter::trace(JSTracer *trc);

  private:
    AutoHashMapRooter(const AutoHashMapRooter &hmr) MOZ_DELETE;
    AutoHashMapRooter &operator=(const AutoHashMapRooter &hmr) MOZ_DELETE;

    HashMapImpl map;

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

template<class T>
class AutoHashSetRooter : protected AutoGCRooter
{
  private:
    typedef js::HashSet<T> HashSetImpl;

  public:
    explicit AutoHashSetRooter(JSContext *cx, ptrdiff_t tag
                               MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, tag), set(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    typedef typename HashSetImpl::Lookup Lookup;
    typedef typename HashSetImpl::Ptr Ptr;
    typedef typename HashSetImpl::AddPtr AddPtr;

    bool init(uint32_t len = 16) {
        return set.init(len);
    }
    bool initialized() const {
        return set.initialized();
    }
    Ptr lookup(const Lookup &l) const {
        return set.lookup(l);
    }
    void remove(Ptr p) {
        set.remove(p);
    }
    AddPtr lookupForAdd(const Lookup &l) const {
        return set.lookupForAdd(l);
    }

    bool add(AddPtr &p, const T &t) {
        return set.add(p, t);
    }

    bool relookupOrAdd(AddPtr &p, const Lookup &l, const T &t) {
        return set.relookupOrAdd(p, l, t);
    }

    typedef typename HashSetImpl::Range Range;
    Range all() const {
        return set.all();
    }

    typedef typename HashSetImpl::Enum Enum;

    void clear() {
        set.clear();
    }

    void finish() {
        set.finish();
    }

    bool empty() const {
        return set.empty();
    }

    uint32_t count() const {
        return set.count();
    }

    size_t capacity() const {
        return set.capacity();
    }

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return set.sizeOfExcludingThis(mallocSizeOf);
    }
    size_t sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        return set.sizeOfIncludingThis(mallocSizeOf);
    }

    unsigned generation() const {
        return set.generation();
    }

    /************************************************** Shorthand operations */

    bool has(const Lookup &l) const {
        return set.has(l);
    }

    bool put(const T &t) {
        return set.put(t);
    }

    bool putNew(const T &t) {
        return set.putNew(t);
    }

    void remove(const Lookup &l) {
        set.remove(l);
    }

    friend void AutoGCRooter::trace(JSTracer *trc);

  private:
    AutoHashSetRooter(const AutoHashSetRooter &hmr) MOZ_DELETE;
    AutoHashSetRooter &operator=(const AutoHashSetRooter &hmr) MOZ_DELETE;

    HashSetImpl set;

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class MOZ_STACK_CLASS AutoValueVector : public AutoVectorRooter<Value>
{
  public:
    explicit AutoValueVector(JSContext *cx
                             MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<Value>(cx, VALVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoIdVector : public AutoVectorRooter<jsid>
{
  public:
    explicit AutoIdVector(JSContext *cx
                          MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<jsid>(cx, IDVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoObjectVector : public AutoVectorRooter<JSObject *>
{
  public:
    explicit AutoObjectVector(JSContext *cx
                              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<JSObject *>(cx, OBJVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoFunctionVector : public AutoVectorRooter<JSFunction *>
{
  public:
    explicit AutoFunctionVector(JSContext *cx
                              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<JSFunction *>(cx, FUNVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

class AutoScriptVector : public AutoVectorRooter<JSScript *>
{
  public:
    explicit AutoScriptVector(JSContext *cx
                              MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
        : AutoVectorRooter<JSScript *>(cx, SCRIPTVECTOR)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/*
 * Cutsom rooting behavior for internal and external clients.
 */
class JS_PUBLIC_API(CustomAutoRooter) : private AutoGCRooter
{
  public:
    explicit CustomAutoRooter(JSContext *cx MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, CUSTOM)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    friend void AutoGCRooter::trace(JSTracer *trc);

  protected:
    /* Supplied by derived class to trace roots. */
    virtual void trace(JSTracer *trc) = 0;

  private:
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

/* Returns true if |v| is considered an acceptable this-value. */
typedef bool (*IsAcceptableThis)(const Value &v);

/*
 * Implements the guts of a method; guaranteed to be provided an acceptable
 * this-value, as determined by a corresponding IsAcceptableThis method.
 */
typedef bool (*NativeImpl)(JSContext *cx, CallArgs args);

namespace detail {

/* DON'T CALL THIS DIRECTLY.  It's for use only by CallNonGenericMethod! */
extern JS_PUBLIC_API(bool)
CallMethodIfWrapped(JSContext *cx, IsAcceptableThis test, NativeImpl impl, CallArgs args);

} /* namespace detail */

/*
 * Methods usually act upon |this| objects only from a single global object and
 * compartment.  Sometimes, however, a method must act upon |this| values from
 * multiple global objects or compartments.  In such cases the |this| value a
 * method might see will be wrapped, such that various access to the object --
 * to its class, its private data, its reserved slots, and so on -- will not
 * work properly without entering that object's compartment.  This method
 * implements a solution to this problem.
 *
 * To implement a method that accepts |this| values from multiple compartments,
 * define two functions.  The first function matches the IsAcceptableThis type
 * and indicates whether the provided value is an acceptable |this| for the
 * method; it must be a pure function only of its argument.
 *
 *   static JSClass AnswerClass = { ... };
 *
 *   static bool
 *   IsAnswerObject(const Value &v)
 *   {
 *       if (!v.isObject())
 *           return false;
 *       return JS_GetClass(&v.toObject()) == &AnswerClass;
 *   }
 *
 * The second function implements the NativeImpl signature and defines the
 * behavior of the method when it is provided an acceptable |this| value.
 * Aside from some typing niceties -- see the CallArgs interface for details --
 * its interface is the same as that of JSNative.
 *
 *   static bool
 *   answer_getAnswer_impl(JSContext *cx, JS::CallArgs args)
 *   {
 *       args.rval().setInt32(42);
 *       return true;
 *   }
 *
 * The implementation function is guaranteed to be called *only* with a |this|
 * value which is considered acceptable.
 *
 * Now to implement the actual method, write a JSNative that calls the method
 * declared below, passing the appropriate template and runtime arguments.
 *
 *   static JSBool
 *   answer_getAnswer(JSContext *cx, unsigned argc, JS::Value *vp)
 *   {
 *       JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
 *       return JS::CallNonGenericMethod<IsAnswerObject, answer_getAnswer_impl>(cx, args);
 *   }
 *
 * Note that, because they are used as template arguments, the predicate
 * and implementation functions must have external linkage. (This is
 * unfortunate, but GCC wasn't inlining things as one would hope when we
 * passed them as function arguments.)
 *
 * JS::CallNonGenericMethod will test whether |args.thisv()| is acceptable.  If
 * it is, it will call the provided implementation function, which will return
 * a value and indicate success.  If it is not, it will attempt to unwrap
 * |this| and call the implementation function on the unwrapped |this|.  If
 * that succeeds, all well and good.  If it doesn't succeed, a TypeError will
 * be thrown.
 *
 * Note: JS::CallNonGenericMethod will only work correctly if it's called in
 *       tail position in a JSNative.  Do not call it from any other place.
 */
template<IsAcceptableThis Test, NativeImpl Impl>
JS_ALWAYS_INLINE bool
CallNonGenericMethod(JSContext *cx, CallArgs args)
{
    const Value &thisv = args.thisv();
    if (Test(thisv))
        return Impl(cx, args);

    return detail::CallMethodIfWrapped(cx, Test, Impl, args);
}

JS_ALWAYS_INLINE bool
CallNonGenericMethod(JSContext *cx, IsAcceptableThis Test, NativeImpl Impl, CallArgs args)
{
    const Value &thisv = args.thisv();
    if (Test(thisv))
        return Impl(cx, args);

    return detail::CallMethodIfWrapped(cx, Test, Impl, args);
}

}  /* namespace JS */

/************************************************************************/

/* JSClass operation signatures. */

/*
 * Add or get a property named by id in obj.  Note the jsid id type -- id may
 * be a string (Unicode property identifier) or an int (element index).  The
 * *vp out parameter, on success, is the new property value after the action.
 */
typedef JSBool
(* JSPropertyOp)(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id, JS::MutableHandle<JS::Value> vp);

/*
 * Set a property named by id in obj, treating the assignment as strict
 * mode code if strict is true. Note the jsid id type -- id may be a string
 * (Unicode property identifier) or an int (element index). The *vp out
 * parameter, on success, is the new property value after the
 * set.
 */
typedef JSBool
(* JSStrictPropertyOp)(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id, JSBool strict, JS::MutableHandle<JS::Value> vp);

/*
 * Delete a property named by id in obj.
 *
 * If an error occurred, return false as per normal JSAPI error practice.
 *
 * If no error occurred, but the deletion attempt wasn't allowed (perhaps
 * because the property was non-configurable), set *succeeded to false and
 * return true.  This will cause |delete obj[id]| to evaluate to false in
 * non-strict mode code, and to throw a TypeError in strict mode code.
 *
 * If no error occurred and the deletion wasn't disallowed (this is *not* the
 * same as saying that a deletion actually occurred -- deleting a non-existent
 * property, or an inherited property, is allowed -- it's just pointless),
 * set *succeeded to true and return true.
 */
typedef JSBool
(* JSDeletePropertyOp)(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id, JSBool *succeeded);

/*
 * This function type is used for callbacks that enumerate the properties of
 * a JSObject.  The behavior depends on the value of enum_op:
 *
 *  JSENUMERATE_INIT
 *    A new, opaque iterator state should be allocated and stored in *statep.
 *    (You can use PRIVATE_TO_JSVAL() to tag the pointer to be stored).
 *
 *    The number of properties that will be enumerated should be returned as
 *    an integer jsval in *idp, if idp is non-null, and provided the number of
 *    enumerable properties is known.  If idp is non-null and the number of
 *    enumerable properties can't be computed in advance, *idp should be set
 *    to JSVAL_ZERO.
 *
 *  JSENUMERATE_INIT_ALL
 *    Used identically to JSENUMERATE_INIT, but exposes all properties of the
 *    object regardless of enumerability.
 *
 *  JSENUMERATE_NEXT
 *    A previously allocated opaque iterator state is passed in via statep.
 *    Return the next jsid in the iteration using *idp.  The opaque iterator
 *    state pointed at by statep is destroyed and *statep is set to JSVAL_NULL
 *    if there are no properties left to enumerate.
 *
 *  JSENUMERATE_DESTROY
 *    Destroy the opaque iterator state previously allocated in *statep by a
 *    call to this function when enum_op was JSENUMERATE_INIT or
 *    JSENUMERATE_INIT_ALL.
 *
 * The return value is used to indicate success, with a value of JS_FALSE
 * indicating failure.
 */
typedef JSBool
(* JSNewEnumerateOp)(JSContext *cx, JS::Handle<JSObject*> obj, JSIterateOp enum_op,
                     JS::MutableHandle<JS::Value> statep, JS::MutableHandleId idp);

/*
 * The old-style JSClass.enumerate op should define all lazy properties not
 * yet reflected in obj.
 */
typedef JSBool
(* JSEnumerateOp)(JSContext *cx, JS::Handle<JSObject*> obj);

/*
 * Resolve a lazy property named by id in obj by defining it directly in obj.
 * Lazy properties are those reflected from some peer native property space
 * (e.g., the DOM attributes for a given node reflected as obj) on demand.
 *
 * JS looks for a property in an object, and if not found, tries to resolve
 * the given id.  If resolve succeeds, the engine looks again in case resolve
 * defined obj[id].  If no such property exists directly in obj, the process
 * is repeated with obj's prototype, etc.
 *
 * NB: JSNewResolveOp provides a cheaper way to resolve lazy properties.
 */
typedef JSBool
(* JSResolveOp)(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id);

/*
 * Like JSResolveOp, but flags provide contextual information as follows:
 *
 *  JSRESOLVE_ASSIGNING   obj[id] is on the left-hand side of an assignment
 *
 * The *objp out parameter, on success, should be null to indicate that id
 * was not resolved; and non-null, referring to obj or one of its prototypes,
 * if id was resolved.  The hook may assume *objp is null on entry.
 *
 * This hook instead of JSResolveOp is called via the JSClass.resolve member
 * if JSCLASS_NEW_RESOLVE is set in JSClass.flags.
 */
typedef JSBool
(* JSNewResolveOp)(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id, unsigned flags,
                   JS::MutableHandleObject objp);

/*
 * Convert obj to the given type, returning true with the resulting value in
 * *vp on success, and returning false on error or exception.
 */
typedef JSBool
(* JSConvertOp)(JSContext *cx, JS::Handle<JSObject*> obj, JSType type,
                JS::MutableHandle<JS::Value> vp);

typedef struct JSFreeOp JSFreeOp;

struct JSFreeOp {
  private:
    JSRuntime   *runtime_;

  protected:
    JSFreeOp(JSRuntime *rt)
      : runtime_(rt) { }

  public:
    JSRuntime *runtime() const {
        return runtime_;
    }
};

/*
 * Finalize obj, which the garbage collector has determined to be unreachable
 * from other live objects or from GC roots.  Obviously, finalizers must never
 * store a reference to obj.
 */
typedef void
(* JSFinalizeOp)(JSFreeOp *fop, JSObject *obj);

/*
 * Finalizes external strings created by JS_NewExternalString.
 */
typedef struct JSStringFinalizer JSStringFinalizer;

struct JSStringFinalizer {
    void (*finalize)(const JSStringFinalizer *fin, jschar *chars);
};

/*
 * JSClass.checkAccess type: check whether obj[id] may be accessed per mode,
 * returning false on error/exception, true on success with obj[id]'s last-got
 * value in *vp, and its attributes in *attrsp.  As for JSPropertyOp above, id
 * is either a string or an int jsval.
 */
typedef JSBool
(* JSCheckAccessOp)(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
                    JSAccessMode mode, JS::MutableHandle<JS::Value> vp);

/*
 * Check whether v is an instance of obj.  Return false on error or exception,
 * true on success with JS_TRUE in *bp if v is an instance of obj, JS_FALSE in
 * *bp otherwise.
 */
typedef JSBool
(* JSHasInstanceOp)(JSContext *cx, JS::Handle<JSObject*> obj, JS::MutableHandle<JS::Value> vp,
                    JSBool *bp);

/*
 * Function type for trace operation of the class called to enumerate all
 * traceable things reachable from obj's private data structure. For each such
 * thing, a trace implementation must call one of the JS_Call*Tracer variants
 * on the thing.
 *
 * JSTraceOp implementation can assume that no other threads mutates object
 * state. It must not change state of the object or corresponding native
 * structures. The only exception for this rule is the case when the embedding
 * needs a tight integration with GC. In that case the embedding can check if
 * the traversal is a part of the marking phase through calling
 * JS_IsGCMarkingTracer and apply a special code like emptying caches or
 * marking its native structures.
 */
typedef void
(* JSTraceOp)(JSTracer *trc, JSObject *obj);

/*
 * Callback that JSTraceOp implementation can provide to return a string
 * describing the reference traced with JS_CallTracer.
 */
typedef void
(* JSTraceNamePrinter)(JSTracer *trc, char *buf, size_t bufsize);

typedef JSObject *
(* JSWeakmapKeyDelegateOp)(JSObject *obj);

/* Callbacks and their arguments. */

typedef enum JSContextOp {
    JSCONTEXT_NEW,
    JSCONTEXT_DESTROY
} JSContextOp;

/*
 * The possible values for contextOp when the runtime calls the callback are:
 *   JSCONTEXT_NEW      JS_NewContext successfully created a new JSContext
 *                      instance. The callback can initialize the instance as
 *                      required. If the callback returns false, the instance
 *                      will be destroyed and JS_NewContext returns null. In
 *                      this case the callback is not called again.
 *   JSCONTEXT_DESTROY  One of JS_DestroyContext* methods is called. The
 *                      callback may perform its own cleanup and must always
 *                      return true.
 *   Any other value    For future compatibility the callback must do nothing
 *                      and return true in this case.
 */
typedef JSBool
(* JSContextCallback)(JSContext *cx, unsigned contextOp);

typedef enum JSGCStatus {
    JSGC_BEGIN,
    JSGC_END
} JSGCStatus;

typedef void
(* JSGCCallback)(JSRuntime *rt, JSGCStatus status);

typedef enum JSFinalizeStatus {
    /*
     * Called when preparing to sweep a group of compartments, before anything
     * has been swept.  The collector will not yield to the mutator before
     * calling the callback with JSFINALIZE_GROUP_END status.
     */
    JSFINALIZE_GROUP_START,

    /*
     * Called when preparing to sweep a group of compartments. Weak references
     * to unmarked things have been removed and things that are not swept
     * incrementally have been finalized at this point.  The collector may yield
     * to the mutator after this point.
     */
    JSFINALIZE_GROUP_END,

    /*
     * Called at the end of collection when everything has been swept.
     */
    JSFINALIZE_COLLECTION_END
} JSFinalizeStatus;

typedef void
(* JSFinalizeCallback)(JSFreeOp *fop, JSFinalizeStatus status, JSBool isCompartment);

/*
 * Generic trace operation that calls JS_CallTracer on each traceable thing
 * stored in data.
 */
typedef void
(* JSTraceDataOp)(JSTracer *trc, void *data);

typedef JSBool
(* JSOperationCallback)(JSContext *cx);

typedef void
(* JSErrorReporter)(JSContext *cx, const char *message, JSErrorReport *report);

#ifdef MOZ_TRACE_JSCALLS
typedef void
(* JSFunctionCallback)(const JSFunction *fun,
                       const JSScript *scr,
                       const JSContext *cx,
                       int entering);
#endif

/*
 * Possible exception types. These types are part of a JSErrorFormatString
 * structure. They define which error to throw in case of a runtime error.
 * JSEXN_NONE marks an unthrowable error.
 */
typedef enum JSExnType {
    JSEXN_NONE = -1,
      JSEXN_ERR,
        JSEXN_INTERNALERR,
        JSEXN_EVALERR,
        JSEXN_RANGEERR,
        JSEXN_REFERENCEERR,
        JSEXN_SYNTAXERR,
        JSEXN_TYPEERR,
        JSEXN_URIERR,
        JSEXN_LIMIT
} JSExnType;

typedef struct JSErrorFormatString {
    /* The error format string in ASCII. */
    const char *format;

    /* The number of arguments to expand in the formatted error message. */
    uint16_t argCount;

    /* One of the JSExnType constants above. */
    int16_t exnType;
} JSErrorFormatString;

typedef const JSErrorFormatString *
(* JSErrorCallback)(void *userRef, const char *locale,
                    const unsigned errorNumber);

typedef JSBool
(* JSLocaleToUpperCase)(JSContext *cx, JS::Handle<JSString*> src,
                        JS::MutableHandle<JS::Value> rval);

typedef JSBool
(* JSLocaleToLowerCase)(JSContext *cx, JS::Handle<JSString*> src,
                        JS::MutableHandle<JS::Value> rval);

typedef JSBool
(* JSLocaleCompare)(JSContext *cx, JS::Handle<JSString*> src1, JS::Handle<JSString*> src2,
                    JS::MutableHandle<JS::Value> rval);

typedef JSBool
(* JSLocaleToUnicode)(JSContext *cx, const char *src, JS::MutableHandle<JS::Value> rval);

/*
 * Security protocol types.
 */

typedef void
(* JSDestroyPrincipalsOp)(JSPrincipals *principals);

/*
 * Used to check if a CSP instance wants to disable eval() and friends.
 * See js_CheckCSPPermitsJSAction() in jsobj.
 */
typedef JSBool
(* JSCSPEvalChecker)(JSContext *cx);

/*
 * Used to check if an xss filter wants to disable eval() and friends.
 * See js_CheckXSSPermitsJSAction() in jsobj.
 */
typedef JSBool
(* JSXSSFilterChecker)(JSContext *cx, JSString *str);

/*
 * Callback used to ask the embedding for the cross compartment wrapper handler
 * that implements the desired prolicy for this kind of object in the
 * destination compartment. |obj| is the object to be wrapped. If |existing| is
 * non-NULL, it will point to an existing wrapper object that should be re-used
 * if possible. |existing| is guaranteed to be a cross-compartment wrapper with
 * a lazily-defined prototype and the correct global. It is guaranteed not to
 * wrap a function.
 */
typedef JSObject *
(* JSWrapObjectCallback)(JSContext *cx, JS::HandleObject existing, JS::HandleObject obj,
                         JS::HandleObject proto, JS::HandleObject parent,
                         unsigned flags);

/*
 * Callback used by the wrap hook to ask the embedding to prepare an object
 * for wrapping in a context. This might include unwrapping other wrappers
 * or even finding a more suitable object for the new compartment.
 */
typedef JSObject *
(* JSPreWrapCallback)(JSContext *cx, JS::HandleObject scope, JS::HandleObject obj,
                      unsigned flags);

/*
 * Callback used when wrapping determines that the underlying object is already
 * in the compartment for which it is being wrapped. This allows consumers to
 * maintain same-compartment wrapping invariants.
 *
 * |obj| is guaranteed to be same-compartment as |cx|, but it may (or may not)
 * be a security or cross-compartment wrapper. This is an unfortunate contract,
 * but is important for to avoid unnecessarily recomputing every cross-
 * compartment wrapper that gets passed to wrap.
 */
typedef JSObject *
(* JSSameCompartmentWrapObjectCallback)(JSContext *cx, JS::HandleObject obj);

typedef void
(* JSDestroyCompartmentCallback)(JSFreeOp *fop, JSCompartment *compartment);

typedef void
(* JSZoneCallback)(JS::Zone *zone);

typedef void
(* JSCompartmentNameCallback)(JSRuntime *rt, JSCompartment *compartment,
                              char *buf, size_t bufsize);

/*
 * Read structured data from the reader r. This hook is used to read a value
 * previously serialized by a call to the WriteStructuredCloneOp hook.
 *
 * tag and data are the pair of uint32_t values from the header. The callback
 * may use the JS_Read* APIs to read any other relevant parts of the object
 * from the reader r. closure is any value passed to the JS_ReadStructuredClone
 * function. Return the new object on success, NULL on error/exception.
 */
typedef JSObject *(*ReadStructuredCloneOp)(JSContext *cx, JSStructuredCloneReader *r,
                                           uint32_t tag, uint32_t data, void *closure);

/*
 * Structured data serialization hook. The engine can write primitive values,
 * Objects, Arrays, Dates, RegExps, TypedArrays, and ArrayBuffers. Any other
 * type of object requires application support. This callback must first use
 * the JS_WriteUint32Pair API to write an object header, passing a value
 * greater than JS_SCTAG_USER to the tag parameter. Then it can use the
 * JS_Write* APIs to write any other relevant parts of the value v to the
 * writer w. closure is any value passed to the JS_WriteStructuredCLone function.
 *
 * Return true on success, false on error/exception.
 */
typedef JSBool (*WriteStructuredCloneOp)(JSContext *cx, JSStructuredCloneWriter *w,
                                         JS::Handle<JSObject*> obj, void *closure);

/*
 * This is called when JS_WriteStructuredClone is given an invalid transferable.
 * To follow HTML5, the application must throw a DATA_CLONE_ERR DOMException
 * with error set to one of the JS_SCERR_* values.
 */
typedef void (*StructuredCloneErrorOp)(JSContext *cx, uint32_t errorid);

/************************************************************************/

/*
 * JS constants. For efficiency, prefer predicates (e.g. v.isNull()) and
 * constructing values from scratch (e.g. Int32Value(0)).  These constants are
 * stored in memory and initialized at startup, so testing against them and
 * using them requires memory loads and will be correspondingly slow.
 */
extern JS_PUBLIC_DATA(const jsval) JSVAL_NULL;
extern JS_PUBLIC_DATA(const jsval) JSVAL_ZERO;
extern JS_PUBLIC_DATA(const jsval) JSVAL_ONE;
extern JS_PUBLIC_DATA(const jsval) JSVAL_FALSE;
extern JS_PUBLIC_DATA(const jsval) JSVAL_TRUE;
extern JS_PUBLIC_DATA(const jsval) JSVAL_VOID;

static JS_ALWAYS_INLINE jsval
JS_NumberValue(double d)
{
    int32_t i;
    d = JS::CanonicalizeNaN(d);
    if (mozilla::DoubleIsInt32(d, &i))
        return INT_TO_JSVAL(i);
    return DOUBLE_TO_JSVAL(d);
}

/************************************************************************/

/*
 * A jsid is an identifier for a property or method of an object which is
 * either a 31-bit signed integer, interned string or object.  Also, there is
 * an additional jsid value, JSID_VOID, which does not occur in JS scripts but
 * may be used to indicate the absence of a valid jsid.
 *
 * A jsid is not implicitly convertible to or from a jsval; JS_ValueToId or
 * JS_IdToValue must be used instead.
 */

#define JSID_TYPE_STRING                 0x0
#define JSID_TYPE_INT                    0x1
#define JSID_TYPE_VOID                   0x2
#define JSID_TYPE_OBJECT                 0x4
#define JSID_TYPE_MASK                   0x7

/*
 * Avoid using canonical 'id' for jsid parameters since this is a magic word in
 * Objective-C++ which, apparently, wants to be able to #include jsapi.h.
 */
#define id iden

static JS_ALWAYS_INLINE JSBool
JSID_IS_STRING(jsid id)
{
    return (JSID_BITS(id) & JSID_TYPE_MASK) == 0;
}

static JS_ALWAYS_INLINE JSString *
JSID_TO_STRING(jsid id)
{
    JS_ASSERT(JSID_IS_STRING(id));
    return (JSString *)JSID_BITS(id);
}

static JS_ALWAYS_INLINE JSBool
JSID_IS_ZERO(jsid id)
{
    return JSID_BITS(id) == 0;
}

JS_PUBLIC_API(JSBool)
JS_StringHasBeenInterned(JSContext *cx, JSString *str);

/*
 * Only JSStrings that have been interned via the JSAPI can be turned into
 * jsids by API clients.
 *
 * N.B. if a jsid is backed by a string which has not been interned, that
 * string must be appropriately rooted to avoid being collected by the GC.
 */
JS_PUBLIC_API(jsid)
INTERNED_STRING_TO_JSID(JSContext *cx, JSString *str);

static JS_ALWAYS_INLINE JSBool
JSID_IS_INT(jsid id)
{
    return !!(JSID_BITS(id) & JSID_TYPE_INT);
}

static JS_ALWAYS_INLINE int32_t
JSID_TO_INT(jsid id)
{
    JS_ASSERT(JSID_IS_INT(id));
    return ((uint32_t)JSID_BITS(id)) >> 1;
}

#define JSID_INT_MIN  0
#define JSID_INT_MAX  INT32_MAX

static JS_ALWAYS_INLINE JSBool
INT_FITS_IN_JSID(int32_t i)
{
    return i >= 0;
}

static JS_ALWAYS_INLINE jsid
INT_TO_JSID(int32_t i)
{
    jsid id;
    JS_ASSERT(INT_FITS_IN_JSID(i));
    JSID_BITS(id) = ((i << 1) | JSID_TYPE_INT);
    return id;
}

static JS_ALWAYS_INLINE JSBool
JSID_IS_OBJECT(jsid id)
{
    return (JSID_BITS(id) & JSID_TYPE_MASK) == JSID_TYPE_OBJECT &&
           (size_t)JSID_BITS(id) != JSID_TYPE_OBJECT;
}

static JS_ALWAYS_INLINE JSObject *
JSID_TO_OBJECT(jsid id)
{
    JS_ASSERT(JSID_IS_OBJECT(id));
    return (JSObject *)(JSID_BITS(id) & ~(size_t)JSID_TYPE_MASK);
}

static JS_ALWAYS_INLINE jsid
OBJECT_TO_JSID(JSObject *obj)
{
    jsid id;
    JS_ASSERT(obj != NULL);
    JS_ASSERT(((size_t)obj & JSID_TYPE_MASK) == 0);
    JSID_BITS(id) = ((size_t)obj | JSID_TYPE_OBJECT);
    return id;
}

static JS_ALWAYS_INLINE JSBool
JSID_IS_GCTHING(jsid id)
{
    return JSID_IS_STRING(id) || JSID_IS_OBJECT(id);
}

static JS_ALWAYS_INLINE void *
JSID_TO_GCTHING(jsid id)
{
    return (void *)(JSID_BITS(id) & ~(size_t)JSID_TYPE_MASK);
}

/*
 * A void jsid is not a valid id and only arises as an exceptional API return
 * value, such as in JS_NextProperty. Embeddings must not pass JSID_VOID into
 * JSAPI entry points expecting a jsid and do not need to handle JSID_VOID in
 * hooks receiving a jsid except when explicitly noted in the API contract.
 */

static JS_ALWAYS_INLINE JSBool
JSID_IS_VOID(const jsid id)
{
    JS_ASSERT_IF(((size_t)JSID_BITS(id) & JSID_TYPE_MASK) == JSID_TYPE_VOID,
                 JSID_BITS(id) == JSID_TYPE_VOID);
    return ((size_t)JSID_BITS(id) == JSID_TYPE_VOID);
}

static JS_ALWAYS_INLINE JSBool
JSID_IS_EMPTY(const jsid id)
{
    return ((size_t)JSID_BITS(id) == JSID_TYPE_OBJECT);
}

#undef id

#ifdef JS_USE_JSID_STRUCT_TYPES
extern JS_PUBLIC_DATA(const jsid) JSID_VOID;
extern JS_PUBLIC_DATA(const jsid) JSID_EMPTY;
#else
# define JSID_VOID ((jsid)JSID_TYPE_VOID)
# define JSID_EMPTY ((jsid)JSID_TYPE_OBJECT)
#endif

/*
 * Returns true iff the given jsval is immune to GC and can be used across
 * multiple JSRuntimes without requiring any conversion API.
 */
static JS_ALWAYS_INLINE JSBool
JSVAL_IS_UNIVERSAL(jsval v)
{
    return !JSVAL_IS_GCTHING(v);
}

namespace JS {

class AutoIdRooter : private AutoGCRooter
{
  public:
    explicit AutoIdRooter(JSContext *cx, jsid aId = INT_TO_JSID(0)
                          MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, ID), id_(aId)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    jsid id() {
        return id_;
    }

    jsid * addr() {
        return &id_;
    }

    friend void AutoGCRooter::trace(JSTracer *trc);

  private:
    jsid id_;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

} /* namespace JS */

/************************************************************************/

/* Property attributes, set in JSPropertySpec and passed to API functions. */
#define JSPROP_ENUMERATE        0x01    /* property is visible to for/in loop */
#define JSPROP_READONLY         0x02    /* not settable: assignment is no-op.
                                           This flag is only valid when neither
                                           JSPROP_GETTER nor JSPROP_SETTER is
                                           set. */
#define JSPROP_PERMANENT        0x04    /* property cannot be deleted */
#define JSPROP_NATIVE_ACCESSORS 0x08    /* set in JSPropertyDescriptor.flags
                                           if getters/setters are JSNatives */
#define JSPROP_GETTER           0x10    /* property holds getter function */
#define JSPROP_SETTER           0x20    /* property holds setter function */
#define JSPROP_SHARED           0x40    /* don't allocate a value slot for this
                                           property; don't copy the property on
                                           set of the same-named property in an
                                           object that delegates to a prototype
                                           containing this property */
#define JSPROP_INDEX            0x80    /* name is actually (int) index */
#define JSPROP_SHORTID         0x100    /* set in JS_DefineProperty attrs
                                           if getters/setters use a shortid */

#define JSFUN_STUB_GSOPS       0x200    /* use JS_PropertyStub getter/setter
                                           instead of defaulting to class gsops
                                           for property holding function */

#define JSFUN_CONSTRUCTOR      0x400    /* native that can be called as a ctor */

/*
 * The following property flags fall outside of the uint8 space used for some
 * of the data structures to store this information. Keep this in mind if
 * ever reorganizing these bitfield values
 */
#define JSPROP_IGNORE_ENUMERATE 0x1000  /* Ignore the value in JSPROP_ENUMERATE.
                                           This flag is only valid when defining over
                                           an existing property. */
#define JSPROP_IGNORE_READONLY  0x2000  /* Ignore the value in JSPROP_READONLY.
                                           This flag is only valid when defining over
                                           an existing property. */
#define JSPROP_IGNORE_PERMANENT 0x4000  /* Ignore the value in JSPROP_PERMANENT.
                                           This flag is only valid when defining over
                                           an existing property. */
#define JSPROP_IGNORE_VALUE     0x8000  /* Ignore the Value in the descriptor. Nothing was
                                           specified when passed to Object.defineProperty
                                           from script. */

/*
 * Specify a generic native prototype methods, i.e., methods of a class
 * prototype that are exposed as static methods taking an extra leading
 * argument: the generic |this| parameter.
 *
 * If you set this flag in a JSFunctionSpec struct's flags initializer, then
 * that struct must live at least as long as the native static method object
 * created due to this flag by JS_DefineFunctions or JS_InitClass.  Typically
 * JSFunctionSpec structs are allocated in static arrays.
 */
#define JSFUN_GENERIC_NATIVE   0x800

#define JSFUN_FLAGS_MASK       0xe00    /* | of all the JSFUN_* flags */

/*
 * The first call to JS_CallOnce by any thread in a process will call 'func'.
 * Later calls to JS_CallOnce with the same JSCallOnceType object will be
 * suppressed.
 *
 * Equivalently: each distinct JSCallOnceType object will allow one JS_CallOnce
 * to invoke its JSInitCallback.
 */
extern JS_PUBLIC_API(JSBool)
JS_CallOnce(JSCallOnceType *once, JSInitCallback func);

/* Microseconds since the epoch, midnight, January 1, 1970 UTC. */
extern JS_PUBLIC_API(int64_t)
JS_Now(void);

/* Don't want to export data, so provide accessors for non-inline jsvals. */
extern JS_PUBLIC_API(jsval)
JS_GetNaNValue(JSContext *cx);

extern JS_PUBLIC_API(jsval)
JS_GetNegativeInfinityValue(JSContext *cx);

extern JS_PUBLIC_API(jsval)
JS_GetPositiveInfinityValue(JSContext *cx);

extern JS_PUBLIC_API(jsval)
JS_GetEmptyStringValue(JSContext *cx);

extern JS_PUBLIC_API(JSString *)
JS_GetEmptyString(JSRuntime *rt);

/*
 * Format is a string of the following characters (spaces are insignificant),
 * specifying the tabulated type conversions:
 *
 *   b      JSBool          Boolean
 *   c      uint16_t/jschar ECMA uint16_t, Unicode char
 *   i      int32_t         ECMA int32_t
 *   u      uint32_t        ECMA uint32_t
 *   j      int32_t         Rounded int32_t (coordinate)
 *   d      double          IEEE double
 *   I      double          Integral IEEE double
 *   S      JSString *      Unicode string, accessed by a JSString pointer
 *   W      jschar *        Unicode character vector, 0-terminated (W for wide)
 *   o      JSObject *      Object reference
 *   f      JSFunction *    Function private
 *   v      jsval           Argument value (no conversion)
 *   *      N/A             Skip this argument (no vararg)
 *   /      N/A             End of required arguments
 *
 * The variable argument list after format must consist of &b, &c, &s, e.g.,
 * where those variables have the types given above.  For the pointer types
 * char *, JSString *, and JSObject *, the pointed-at memory returned belongs
 * to the JS runtime, not to the calling native code.  The runtime promises
 * to keep this memory valid so long as argv refers to allocated stack space
 * (so long as the native function is active).
 *
 * Fewer arguments than format specifies may be passed only if there is a /
 * in format after the last required argument specifier and argc is at least
 * the number of required arguments.  More arguments than format specifies
 * may be passed without error; it is up to the caller to deal with trailing
 * unconverted arguments.
 */
extern JS_PUBLIC_API(JSBool)
JS_ConvertArguments(JSContext *cx, unsigned argc, jsval *argv, const char *format,
                    ...);

#ifdef va_start
extern JS_PUBLIC_API(JSBool)
JS_ConvertArgumentsVA(JSContext *cx, unsigned argc, jsval *argv,
                      const char *format, va_list ap);
#endif

extern JS_PUBLIC_API(JSBool)
JS_ConvertValue(JSContext *cx, jsval v, JSType type, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_ValueToObject(JSContext *cx, jsval v, JSObject **objp);

extern JS_PUBLIC_API(JSFunction *)
JS_ValueToFunction(JSContext *cx, jsval v);

extern JS_PUBLIC_API(JSFunction *)
JS_ValueToConstructor(JSContext *cx, jsval v);

extern JS_PUBLIC_API(JSString *)
JS_ValueToString(JSContext *cx, jsval v);

extern JS_PUBLIC_API(JSString *)
JS_ValueToSource(JSContext *cx, jsval v);

extern JS_PUBLIC_API(JSBool)
JS_ValueToNumber(JSContext *cx, jsval v, double *dp);

namespace js {
/*
 * DO NOT CALL THIS.  Use JS::ToNumber
 */
extern JS_PUBLIC_API(bool)
ToNumberSlow(JSContext *cx, JS::Value v, double *dp);

/*
 * DO NOT CALL THIS. Use JS::ToBoolean
 */
extern JS_PUBLIC_API(bool)
ToBooleanSlow(const JS::Value &v);
} /* namespace js */

namespace JS {

/* ES5 9.3 ToNumber. */
JS_ALWAYS_INLINE bool
ToNumber(JSContext *cx, const Value &v, double *out)
{
    AssertArgumentsAreSane(cx, v);
    {
        js::SkipRoot root(cx, &v);
        js::MaybeCheckStackRoots(cx);
    }

    if (v.isNumber()) {
        *out = v.toNumber();
        return true;
    }
    return js::ToNumberSlow(cx, v, out);
}

JS_ALWAYS_INLINE bool
ToBoolean(const Value &v)
{
    if (v.isBoolean())
        return v.toBoolean();
    if (v.isInt32())
        return v.toInt32() != 0;
    if (v.isNullOrUndefined())
        return false;
    if (v.isDouble()) {
        double d = v.toDouble();
        return !mozilla::IsNaN(d) && d != 0;
    }

    /* The slow path handles strings and objects. */
    return js::ToBooleanSlow(v);
}

} /* namespace JS */

extern JS_PUBLIC_API(JSBool)
JS_DoubleIsInt32(double d, int32_t *ip);

extern JS_PUBLIC_API(int32_t)
JS_DoubleToInt32(double d);

extern JS_PUBLIC_API(uint32_t)
JS_DoubleToUint32(double d);

/*
 * Convert a value to a number, then to an int32_t, according to the ECMA rules
 * for ToInt32.
 */
extern JS_PUBLIC_API(JSBool)
JS_ValueToECMAInt32(JSContext *cx, jsval v, int32_t *ip);

/*
 * Convert a value to a number, then to an int64_t, according to the WebIDL
 * rules for ToInt64: http://dev.w3.org/2006/webapi/WebIDL/#es-long-long
 */
extern JS_PUBLIC_API(JSBool)
JS_ValueToInt64(JSContext *cx, jsval v, int64_t *ip);

/*
 * Convert a value to a number, then to an uint64_t, according to the WebIDL
 * rules for ToUint64: http://dev.w3.org/2006/webapi/WebIDL/#es-unsigned-long-long
 */
extern JS_PUBLIC_API(JSBool)
JS_ValueToUint64(JSContext *cx, jsval v, uint64_t *ip);

namespace js {
/* DO NOT CALL THIS.  Use JS::ToInt16. */
extern JS_PUBLIC_API(bool)
ToUint16Slow(JSContext *cx, const JS::Value &v, uint16_t *out);

/* DO NOT CALL THIS.  Use JS::ToInt32. */
extern JS_PUBLIC_API(bool)
ToInt32Slow(JSContext *cx, const JS::Value &v, int32_t *out);

/* DO NOT CALL THIS.  Use JS::ToUint32. */
extern JS_PUBLIC_API(bool)
ToUint32Slow(JSContext *cx, const JS::Value &v, uint32_t *out);

/* DO NOT CALL THIS. Use JS::ToInt64. */
extern JS_PUBLIC_API(bool)
ToInt64Slow(JSContext *cx, const JS::Value &v, int64_t *out);

/* DO NOT CALL THIS. Use JS::ToUint64. */
extern JS_PUBLIC_API(bool)
ToUint64Slow(JSContext *cx, const JS::Value &v, uint64_t *out);
} /* namespace js */

namespace JS {

JS_ALWAYS_INLINE bool
ToUint16(JSContext *cx, const JS::Value &v, uint16_t *out)
{
    AssertArgumentsAreSane(cx, v);
    {
        js::SkipRoot skip(cx, &v);
        js::MaybeCheckStackRoots(cx);
    }

    if (v.isInt32()) {
        *out = uint16_t(v.toInt32());
        return true;
    }
    return js::ToUint16Slow(cx, v, out);
}

JS_ALWAYS_INLINE bool
ToInt32(JSContext *cx, const JS::Value &v, int32_t *out)
{
    AssertArgumentsAreSane(cx, v);
    {
        js::SkipRoot root(cx, &v);
        js::MaybeCheckStackRoots(cx);
    }

    if (v.isInt32()) {
        *out = v.toInt32();
        return true;
    }
    return js::ToInt32Slow(cx, v, out);
}

JS_ALWAYS_INLINE bool
ToUint32(JSContext *cx, const JS::Value &v, uint32_t *out)
{
    AssertArgumentsAreSane(cx, v);
    {
        js::SkipRoot root(cx, &v);
        js::MaybeCheckStackRoots(cx);
    }

    if (v.isInt32()) {
        *out = uint32_t(v.toInt32());
        return true;
    }
    return js::ToUint32Slow(cx, v, out);
}

JS_ALWAYS_INLINE bool
ToInt64(JSContext *cx, const JS::Value &v, int64_t *out)
{
    AssertArgumentsAreSane(cx, v);
    {
        js::SkipRoot skip(cx, &v);
        js::MaybeCheckStackRoots(cx);
    }

    if (v.isInt32()) {
        *out = int64_t(v.toInt32());
        return true;
    }

    return js::ToInt64Slow(cx, v, out);
}

JS_ALWAYS_INLINE bool
ToUint64(JSContext *cx, const JS::Value &v, uint64_t *out)
{
    AssertArgumentsAreSane(cx, v);
    {
        js::SkipRoot skip(cx, &v);
        js::MaybeCheckStackRoots(cx);
    }

    if (v.isInt32()) {
        /* Account for sign extension of negatives into the longer 64bit space. */
        *out = uint64_t(int64_t(v.toInt32()));
        return true;
    }

    return js::ToUint64Slow(cx, v, out);
}


} /* namespace JS */

/*
 * Convert a value to a number, then to a uint32_t, according to the ECMA rules
 * for ToUint32.
 */
extern JS_PUBLIC_API(JSBool)
JS_ValueToECMAUint32(JSContext *cx, jsval v, uint32_t *ip);

/*
 * Convert a value to a number, then to an int32_t if it fits by rounding to
 * nearest; but failing with an error report if the double is out of range
 * or unordered.
 */
extern JS_PUBLIC_API(JSBool)
JS_ValueToInt32(JSContext *cx, jsval v, int32_t *ip);

/*
 * ECMA ToUint16, for mapping a jsval to a Unicode point.
 */
extern JS_PUBLIC_API(JSBool)
JS_ValueToUint16(JSContext *cx, jsval v, uint16_t *ip);

extern JS_PUBLIC_API(JSBool)
JS_ValueToBoolean(JSContext *cx, jsval v, JSBool *bp);

extern JS_PUBLIC_API(JSType)
JS_TypeOfValue(JSContext *cx, jsval v);

extern JS_PUBLIC_API(const char *)
JS_GetTypeName(JSContext *cx, JSType type);

extern JS_PUBLIC_API(JSBool)
JS_StrictlyEqual(JSContext *cx, jsval v1, jsval v2, JSBool *equal);

extern JS_PUBLIC_API(JSBool)
JS_LooselyEqual(JSContext *cx, jsval v1, jsval v2, JSBool *equal);

extern JS_PUBLIC_API(JSBool)
JS_SameValue(JSContext *cx, jsval v1, jsval v2, JSBool *same);

/* True iff fun is the global eval function. */
extern JS_PUBLIC_API(JSBool)
JS_IsBuiltinEvalFunction(JSFunction *fun);

/* True iff fun is the Function constructor. */
extern JS_PUBLIC_API(JSBool)
JS_IsBuiltinFunctionConstructor(JSFunction *fun);

/************************************************************************/

/*
 * Initialization, locking, contexts, and memory allocation.
 *
 * It is important that the first runtime and first context be created in a
 * single-threaded fashion, otherwise the behavior of the library is undefined.
 * See: http://developer.mozilla.org/en/docs/Category:JSAPI_Reference
 */

typedef enum JSUseHelperThreads
{
    JS_NO_HELPER_THREADS,
    JS_USE_HELPER_THREADS
} JSUseHelperThreads;


/**
 * Initialize SpiderMonkey, performing one-time initializations for methods
 * that need it (currently only PRMJ_Now).
 * Once this method has succeeded, it is safe to call JS_NewRuntime and other
 * JSAPI methods.
 *
 * This method MUST be called before any other JSAPI method is used on any
 * thread.  Once it has been used, it is safe to call any JSAPI method, and it
 * remains safe to do so until JS_ShutDown is correctly called.
 */
extern JS_PUBLIC_API(JSBool)
JS_Init(void);

/**
 * Destroy free-standing resources allocated by SpiderMonkey, not associated
 * with any runtime, context, or other structure.
 *
 * This method should be called after all other JSAPI data has been properly
 * cleaned up: every new runtime must have been destroyed, every new context
 * must have been destroyed, and so on.  Calling this method before all other
 * resources have been destroyed has undefined behavior.
 *
 * Failure to call this method currently has no adverse effects other than
 * leaking memory from resources allocated in Init().
 *
 * It is safe to call JS_Init again, and then to resume using SpiderMonkey as
 * usual, once this method has returned.
 */
extern JS_PUBLIC_API(void)
JS_ShutDown(void);

extern JS_PUBLIC_API(JSRuntime *)
JS_NewRuntime(uint32_t maxbytes, JSUseHelperThreads useHelperThreads);

extern JS_PUBLIC_API(void)
JS_DestroyRuntime(JSRuntime *rt);

JS_PUBLIC_API(void *)
JS_GetRuntimePrivate(JSRuntime *rt);

extern JS_PUBLIC_API(JSRuntime *)
JS_GetRuntime(JSContext *cx);

JS_PUBLIC_API(void)
JS_SetRuntimePrivate(JSRuntime *rt, void *data);

extern JS_PUBLIC_API(void)
JS_BeginRequest(JSContext *cx);

extern JS_PUBLIC_API(void)
JS_EndRequest(JSContext *cx);

extern JS_PUBLIC_API(JSBool)
JS_IsInRequest(JSRuntime *rt);

namespace JS {

inline bool
IsPoisonedId(jsid iden)
{
    if (JSID_IS_STRING(iden))
        return JS::IsPoisonedPtr(JSID_TO_STRING(iden));
    if (JSID_IS_OBJECT(iden))
        return JS::IsPoisonedPtr(JSID_TO_OBJECT(iden));
    return false;
}

} /* namespace JS */

namespace js {

template <> struct GCMethods<jsid>
{
    static jsid initial() { return JSID_VOID; }
    static ThingRootKind kind() { return THING_ROOT_ID; }
    static bool poisoned(jsid id) { return JS::IsPoisonedId(id); }
    static bool needsPostBarrier(jsid id) { return false; }
#ifdef JSGC_GENERATIONAL
    static void postBarrier(jsid *idp) {}
    static void relocate(jsid *idp) {}
#endif
};

} /* namespace js */

class JSAutoRequest
{
  public:
    JSAutoRequest(JSContext *cx
                  MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : mContext(cx)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
        JS_BeginRequest(mContext);
    }
    ~JSAutoRequest() {
        JS_EndRequest(mContext);
    }

  protected:
    JSContext *mContext;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

#if 0
  private:
    static void *operator new(size_t) CPP_THROW_NEW { return 0; };
    static void operator delete(void *, size_t) { };
#endif
};

class JSAutoCheckRequest
{
  public:
    JSAutoCheckRequest(JSContext *cx
                       MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
    {
#if defined JS_THREADSAFE && defined DEBUG
        mContext = cx;
        JS_ASSERT(JS_IsInRequest(JS_GetRuntime(cx)));
#endif
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    ~JSAutoCheckRequest() {
#if defined JS_THREADSAFE && defined DEBUG
        JS_ASSERT(JS_IsInRequest(JS_GetRuntime(mContext)));
#endif
    }


  private:
#if defined JS_THREADSAFE && defined DEBUG
    JSContext *mContext;
#endif
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER
};

extern JS_PUBLIC_API(JSContextCallback)
JS_SetContextCallback(JSRuntime *rt, JSContextCallback cxCallback);

extern JS_PUBLIC_API(JSContext *)
JS_NewContext(JSRuntime *rt, size_t stackChunkSize);

extern JS_PUBLIC_API(void)
JS_DestroyContext(JSContext *cx);

extern JS_PUBLIC_API(void)
JS_DestroyContextNoGC(JSContext *cx);

extern JS_PUBLIC_API(void *)
JS_GetContextPrivate(JSContext *cx);

extern JS_PUBLIC_API(void)
JS_SetContextPrivate(JSContext *cx, void *data);

extern JS_PUBLIC_API(void *)
JS_GetSecondContextPrivate(JSContext *cx);

extern JS_PUBLIC_API(void)
JS_SetSecondContextPrivate(JSContext *cx, void *data);

extern JS_PUBLIC_API(JSRuntime *)
JS_GetRuntime(JSContext *cx);

extern JS_PUBLIC_API(JSContext *)
JS_ContextIterator(JSRuntime *rt, JSContext **iterp);

extern JS_PUBLIC_API(JSVersion)
JS_GetVersion(JSContext *cx);

// Mutate the version on the compartment. This is generally discouraged, but
// necessary to support the version mutation in the js and xpc shell command
// set.
//
// It would be nice to put this in jsfriendapi, but the linkage requirements
// of the shells make that impossible.
JS_PUBLIC_API(void)
JS_SetVersionForCompartment(JSCompartment *compartment, JSVersion version);

extern JS_PUBLIC_API(const char *)
JS_VersionToString(JSVersion version);

extern JS_PUBLIC_API(JSVersion)
JS_StringToVersion(const char *string);

/*
 * JS options are orthogonal to version, and may be freely composed with one
 * another as well as with version.
 *
 * JSOPTION_VAROBJFIX is recommended -- see the comments associated with the
 * prototypes for JS_ExecuteScript, JS_EvaluateScript, etc.
 */
#define JSOPTION_EXTRA_WARNINGS JS_BIT(0)       /* warn on dubious practices */
#define JSOPTION_WERROR         JS_BIT(1)       /* convert warning to error */
#define JSOPTION_VAROBJFIX      JS_BIT(2)       /* make JS_EvaluateScript use
                                                   the last object on its 'obj'
                                                   param's scope chain as the
                                                   ECMA 'variables object' */
#define JSOPTION_PRIVATE_IS_NSISUPPORTS \
                                JS_BIT(3)       /* context private data points
                                                   to an nsISupports subclass */
#define JSOPTION_COMPILE_N_GO   JS_BIT(4)       /* caller of JS_Compile*Script
                                                   promises to execute compiled
                                                   script once only; enables
                                                   compile-time scope chain
                                                   resolution of consts. */

/* JS_BIT(5) is currently unused. */

/* JS_BIT(6) is currently unused. */

/* JS_BIT(7) is currently unused. */

#define JSOPTION_DONT_REPORT_UNCAUGHT                                   \
                                JS_BIT(8)       /* When returning from the
                                                   outermost API call, prevent
                                                   uncaught exceptions from
                                                   being converted to error
                                                   reports */

/* JS_BIT(9) is currently unused. */

/* JS_BIT(10) is currently unused. */

/* JS_BIT(11) is currently unused. */

#define JSOPTION_NO_SCRIPT_RVAL JS_BIT(12)      /* A promise to the compiler
                                                   that a null rval out-param
                                                   will be passed to each call
                                                   to JS_ExecuteScript. */
#define JSOPTION_UNROOTED_GLOBAL JS_BIT(13)     /* The GC will not root the
                                                   contexts' default compartment
                                                   object, leaving that up to the
                                                   embedding. */

#define JSOPTION_BASELINE       JS_BIT(14)      /* Baseline compiler. */

#define JSOPTION_PCCOUNT        JS_BIT(15)      /* Collect per-op execution counts */

#define JSOPTION_TYPE_INFERENCE JS_BIT(16)      /* Perform type inference. */
#define JSOPTION_STRICT_MODE    JS_BIT(17)      /* Provides a way to force
                                                   strict mode for all code
                                                   without requiring
                                                   "use strict" annotations. */

#define JSOPTION_ION            JS_BIT(18)      /* IonMonkey */

#define JSOPTION_ASMJS          JS_BIT(19)      /* optimizingasm.js compiler */

#define JSOPTION_MASK           JS_BITMASK(20)

extern JS_PUBLIC_API(uint32_t)
JS_GetOptions(JSContext *cx);

extern JS_PUBLIC_API(uint32_t)
JS_SetOptions(JSContext *cx, uint32_t options);

extern JS_PUBLIC_API(uint32_t)
JS_ToggleOptions(JSContext *cx, uint32_t options);

extern JS_PUBLIC_API(void)
JS_SetJitHardening(JSRuntime *rt, JSBool enabled);

extern JS_PUBLIC_API(const char *)
JS_GetImplementationVersion(void);

extern JS_PUBLIC_API(void)
JS_SetDestroyCompartmentCallback(JSRuntime *rt, JSDestroyCompartmentCallback callback);

extern JS_PUBLIC_API(void)
JS_SetDestroyZoneCallback(JSRuntime *rt, JSZoneCallback callback);

extern JS_PUBLIC_API(void)
JS_SetSweepZoneCallback(JSRuntime *rt, JSZoneCallback callback);

extern JS_PUBLIC_API(void)
JS_SetCompartmentNameCallback(JSRuntime *rt, JSCompartmentNameCallback callback);

extern JS_PUBLIC_API(JSWrapObjectCallback)
JS_SetWrapObjectCallbacks(JSRuntime *rt,
                          JSWrapObjectCallback callback,
                          JSSameCompartmentWrapObjectCallback sccallback,
                          JSPreWrapCallback precallback);

extern JS_PUBLIC_API(void)
JS_SetCompartmentPrivate(JSCompartment *compartment, void *data);

extern JS_PUBLIC_API(void *)
JS_GetCompartmentPrivate(JSCompartment *compartment);

extern JS_PUBLIC_API(void)
JS_SetZoneUserData(JS::Zone *zone, void *data);

extern JS_PUBLIC_API(void *)
JS_GetZoneUserData(JS::Zone *zone);

extern JS_PUBLIC_API(JSBool)
JS_WrapObject(JSContext *cx, JSObject **objp);

extern JS_PUBLIC_API(JSBool)
JS_WrapValue(JSContext *cx, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_WrapId(JSContext *cx, jsid *idp);

extern JS_PUBLIC_API(JSObject *)
JS_TransplantObject(JSContext *cx, JS::HandleObject origobj, JS::HandleObject target);

extern JS_PUBLIC_API(JSBool)
JS_RefreshCrossCompartmentWrappers(JSContext *cx, JSObject *ob);

/*
 * At any time, a JSContext has a current (possibly-NULL) compartment.
 * Compartments are described in:
 *
 *   developer.mozilla.org/en-US/docs/SpiderMonkey/SpiderMonkey_compartments
 *
 * The current compartment of a context may be changed. The preferred way to do
 * this is with JSAutoCompartment:
 *
 *   void foo(JSContext *cx, JSObject *obj) {
 *     // in some compartment 'c'
 *     {
 *       JSAutoCompartment ac(cx, obj);  // constructor enters
 *       // in the compartment of 'obj'
 *     }                                 // destructor leaves
 *     // back in compartment 'c'
 *   }
 *
 * For more complicated uses that don't neatly fit in a C++ stack frame, the
 * compartment can entered and left using separate function calls:
 *
 *   void foo(JSContext *cx, JSObject *obj) {
 *     // in 'oldCompartment'
 *     JSCompartment *oldCompartment = JS_EnterCompartment(cx, obj);
 *     // in the compartment of 'obj'
 *     JS_LeaveCompartment(cx, oldCompartment);
 *     // back in 'oldCompartment'
 *   }
 *
 * Note: these calls must still execute in a LIFO manner w.r.t all other
 * enter/leave calls on the context. Furthermore, only the return value of a
 * JS_EnterCompartment call may be passed as the 'oldCompartment' argument of
 * the corresponding JS_LeaveCompartment call.
 */

class JS_PUBLIC_API(JSAutoCompartment)
{
    JSContext *cx_;
    JSCompartment *oldCompartment_;
  public:
    JSAutoCompartment(JSContext *cx, JSObject *target);
    JSAutoCompartment(JSContext *cx, JSScript *target);
    ~JSAutoCompartment();
};

/* NB: This API is infallible; a NULL return value does not indicate error. */
extern JS_PUBLIC_API(JSCompartment *)
JS_EnterCompartment(JSContext *cx, JSObject *target);

extern JS_PUBLIC_API(void)
JS_LeaveCompartment(JSContext *cx, JSCompartment *oldCompartment);

typedef void (*JSIterateCompartmentCallback)(JSRuntime *rt, void *data, JSCompartment *compartment);

/*
 * This function calls |compartmentCallback| on every compartment. Beware that
 * there is no guarantee that the compartment will survive after the callback
 * returns. Also, if the callback can GC, there is no guarantee that every
 * compartment will be visited.
 */
extern JS_PUBLIC_API(void)
JS_IterateCompartments(JSRuntime *rt, void *data,
                       JSIterateCompartmentCallback compartmentCallback);

extern JS_PUBLIC_API(void)
JS_SetGlobalObject(JSContext *cx, JSObject *obj);

/*
 * Initialize standard JS class constructors, prototypes, and any top-level
 * functions and constants associated with the standard classes (e.g. isNaN
 * for Number).
 *
 * NB: This sets cx's global object to obj if it was null.
 */
extern JS_PUBLIC_API(JSBool)
JS_InitStandardClasses(JSContext *cx, JSObject *obj);

/*
 * Resolve id, which must contain either a string or an int, to a standard
 * class name in obj if possible, defining the class's constructor and/or
 * prototype and storing true in *resolved.  If id does not name a standard
 * class or a top-level property induced by initializing a standard class,
 * store false in *resolved and just return true.  Return false on error,
 * as usual for JSBool result-typed API entry points.
 *
 * This API can be called directly from a global object class's resolve op,
 * to define standard classes lazily.  The class's enumerate op should call
 * JS_EnumerateStandardClasses(cx, obj), to define eagerly during for..in
 * loops any classes not yet resolved lazily.
 */
extern JS_PUBLIC_API(JSBool)
JS_ResolveStandardClass(JSContext *cx, JS::HandleObject obj, JS::HandleId id,
                        JSBool *resolved);

extern JS_PUBLIC_API(JSBool)
JS_EnumerateStandardClasses(JSContext *cx, JS::HandleObject obj);

extern JS_PUBLIC_API(JSBool)
JS_GetClassObject(JSContext *cx, JSObject *obj, JSProtoKey key, JSObject **objp);

extern JS_PUBLIC_API(JSBool)
JS_GetClassPrototype(JSContext *cx, JSProtoKey key, JSObject **objp);

extern JS_PUBLIC_API(JSProtoKey)
JS_IdentifyClassPrototype(JSContext *cx, JSObject *obj);

/*
 * Returns the original value of |Function.prototype| from the global object in
 * which |forObj| was created.
 */
extern JS_PUBLIC_API(JSObject *)
JS_GetFunctionPrototype(JSContext *cx, JSObject *forObj);

/*
 * Returns the original value of |Object.prototype| from the global object in
 * which |forObj| was created.
 */
extern JS_PUBLIC_API(JSObject *)
JS_GetObjectPrototype(JSContext *cx, JSObject *forObj);

/*
 * Returns the original value of |Array.prototype| from the global object in
 * which |forObj| was created.
 */
extern JS_PUBLIC_API(JSObject *)
JS_GetArrayPrototype(JSContext *cx, JSObject *forObj);

extern JS_PUBLIC_API(JSObject *)
JS_GetGlobalForObject(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_IsGlobalObject(JSObject *obj);

/*
 * May return NULL, if |c| never had a global (e.g. the atoms compartment), or
 * if |c|'s global has been collected.
 */
extern JS_PUBLIC_API(JSObject *)
JS_GetGlobalForCompartmentOrNull(JSContext *cx, JSCompartment *c);

extern JS_PUBLIC_API(JSObject *)
JS_GetGlobalForScopeChain(JSContext *cx);

/*
 * This method returns the global corresponding to the most recent scripted
 * frame, which may not match the cx's current compartment. This is extremely
 * dangerous, because it can bypass compartment security invariants in subtle
 * ways. To use it safely, the caller must perform a subsequent security
 * check. There is currently only one consumer of this function in Goanna, and
 * it should probably stay that way. If you'd like to use it, please consult
 * the XPConnect module owner first.
 */
extern JS_PUBLIC_API(JSObject *)
JS_GetScriptedGlobal(JSContext *cx);

/*
 * Initialize the 'Reflect' object on a global object.
 */
extern JS_PUBLIC_API(JSObject *)
JS_InitReflect(JSContext *cx, JSObject *global);

#ifdef JS_HAS_CTYPES
/*
 * Initialize the 'ctypes' object on a global variable 'obj'. The 'ctypes'
 * object will be sealed.
 */
extern JS_PUBLIC_API(JSBool)
JS_InitCTypesClass(JSContext *cx, JSObject *global);

/*
 * Convert a unicode string 'source' of length 'slen' to the platform native
 * charset, returning a null-terminated string allocated with JS_malloc. On
 * failure, this function should report an error.
 */
typedef char *
(* JSCTypesUnicodeToNativeFun)(JSContext *cx, const jschar *source, size_t slen);

/*
 * Set of function pointers that ctypes can use for various internal functions.
 * See JS_SetCTypesCallbacks below. Providing NULL for a function is safe,
 * and will result in the applicable ctypes functionality not being available.
 */
struct JSCTypesCallbacks {
    JSCTypesUnicodeToNativeFun unicodeToNative;
};

typedef struct JSCTypesCallbacks JSCTypesCallbacks;

/*
 * Set the callbacks on the provided 'ctypesObj' object. 'callbacks' should be a
 * pointer to static data that exists for the lifetime of 'ctypesObj', but it
 * may safely be altered after calling this function and without having
 * to call this function again.
 */
extern JS_PUBLIC_API(void)
JS_SetCTypesCallbacks(JSObject *ctypesObj, JSCTypesCallbacks *callbacks);
#endif

typedef JSBool
(* JSEnumerateDiagnosticMemoryCallback)(void *ptr, size_t length);

/*
 * Enumerate memory regions that contain diagnostic information
 * intended to be included in crash report minidumps.
 */
extern JS_PUBLIC_API(void)
JS_EnumerateDiagnosticMemoryRegions(JSEnumerateDiagnosticMemoryCallback callback);

extern JS_PUBLIC_API(void *)
JS_malloc(JSContext *cx, size_t nbytes);

extern JS_PUBLIC_API(void *)
JS_realloc(JSContext *cx, void *p, size_t nbytes);

/*
 * A wrapper for js_free(p) that may delay js_free(p) invocation as a
 * performance optimization.
 * cx may be NULL.
 */
extern JS_PUBLIC_API(void)
JS_free(JSContext *cx, void *p);

/*
 * A wrapper for js_free(p) that may delay js_free(p) invocation as a
 * performance optimization as specified by the given JSFreeOp instance.
 */
extern JS_PUBLIC_API(void)
JS_freeop(JSFreeOp *fop, void *p);

extern JS_PUBLIC_API(JSFreeOp *)
JS_GetDefaultFreeOp(JSRuntime *rt);

extern JS_PUBLIC_API(void)
JS_updateMallocCounter(JSContext *cx, size_t nbytes);

extern JS_PUBLIC_API(char *)
JS_strdup(JSContext *cx, const char *s);

/* Duplicate a string.  Does not report an error on failure. */
extern JS_PUBLIC_API(char *)
JS_strdup(JSRuntime *rt, const char *s);


/*
 * A GC root is a pointer to a jsval, JSObject * or JSString * that itself
 * points into the GC heap. JS_AddValueRoot takes a pointer to a jsval and
 * JS_AddGCThingRoot takes a pointer to a JSObject * or JString *.
 *
 * Note that, since JS_Add*Root stores the address of a variable (of type
 * jsval, JSString *, or JSObject *), that variable must live until
 * JS_Remove*Root is called to remove that variable. For example, after:
 *
 *   void some_function() {
 *     jsval v;
 *     JS_AddNamedValueRoot(cx, &v, "name");
 *
 * the caller must perform
 *
 *     JS_RemoveValueRoot(cx, &v);
 *
 * before some_function() returns.
 *
 * Also, use JS_AddNamed*Root(cx, &structPtr->memberObj, "structPtr->memberObj")
 * in preference to JS_Add*Root(cx, &structPtr->memberObj), in order to identify
 * roots by their source callsites.  This way, you can find the callsite while
 * debugging if you should fail to do JS_Remove*Root(cx, &structPtr->memberObj)
 * before freeing structPtr's memory.
 */
extern JS_PUBLIC_API(JSBool)
JS_AddValueRoot(JSContext *cx, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_AddStringRoot(JSContext *cx, JSString **rp);

extern JS_PUBLIC_API(JSBool)
JS_AddObjectRoot(JSContext *cx, JSObject **rp);

#ifdef NAME_ALL_GC_ROOTS
#define JS_DEFINE_TO_TOKEN(def) #def
#define JS_DEFINE_TO_STRING(def) JS_DEFINE_TO_TOKEN(def)
#define JS_AddValueRoot(cx,vp) JS_AddNamedValueRoot((cx), (vp), (__FILE__ ":" JS_TOKEN_TO_STRING(__LINE__))
#define JS_AddStringRoot(cx,rp) JS_AddNamedStringRoot((cx), (rp), (__FILE__ ":" JS_TOKEN_TO_STRING(__LINE__))
#define JS_AddObjectRoot(cx,rp) JS_AddNamedObjectRoot((cx), (rp), (__FILE__ ":" JS_TOKEN_TO_STRING(__LINE__))
#endif

extern JS_PUBLIC_API(JSBool)
JS_AddNamedValueRoot(JSContext *cx, jsval *vp, const char *name);

extern JS_PUBLIC_API(JSBool)
JS_AddNamedValueRootRT(JSRuntime *rt, jsval *vp, const char *name);

extern JS_PUBLIC_API(JSBool)
JS_AddNamedStringRoot(JSContext *cx, JSString **rp, const char *name);

extern JS_PUBLIC_API(JSBool)
JS_AddNamedObjectRoot(JSContext *cx, JSObject **rp, const char *name);

extern JS_PUBLIC_API(JSBool)
JS_AddNamedScriptRoot(JSContext *cx, JSScript **rp, const char *name);

extern JS_PUBLIC_API(void)
JS_RemoveValueRoot(JSContext *cx, jsval *vp);

extern JS_PUBLIC_API(void)
JS_RemoveStringRoot(JSContext *cx, JSString **rp);

extern JS_PUBLIC_API(void)
JS_RemoveObjectRoot(JSContext *cx, JSObject **rp);

extern JS_PUBLIC_API(void)
JS_RemoveScriptRoot(JSContext *cx, JSScript **rp);

extern JS_PUBLIC_API(void)
JS_RemoveValueRootRT(JSRuntime *rt, jsval *vp);

extern JS_PUBLIC_API(void)
JS_RemoveStringRootRT(JSRuntime *rt, JSString **rp);

extern JS_PUBLIC_API(void)
JS_RemoveObjectRootRT(JSRuntime *rt, JSObject **rp);

extern JS_PUBLIC_API(void)
JS_RemoveScriptRootRT(JSRuntime *rt, JSScript **rp);

/* TODO: remove these APIs */

extern JS_FRIEND_API(void)
js_RemoveRoot(JSRuntime *rt, void *rp);

/*
 * C-compatible version of the Anchor class. It should be called after the last
 * use of the variable it protects.
 */
extern JS_NEVER_INLINE JS_PUBLIC_API(void)
JS_AnchorPtr(void *p);

/*
 * Register externally maintained GC roots.
 *
 * traceOp: the trace operation. For each root the implementation should call
 *          JS_CallTracer whenever the root contains a traceable thing.
 * data:    the data argument to pass to each invocation of traceOp.
 */
extern JS_PUBLIC_API(JSBool)
JS_AddExtraGCRootsTracer(JSRuntime *rt, JSTraceDataOp traceOp, void *data);

/* Undo a call to JS_AddExtraGCRootsTracer. */
extern JS_PUBLIC_API(void)
JS_RemoveExtraGCRootsTracer(JSRuntime *rt, JSTraceDataOp traceOp, void *data);

/*
 * JS_CallTracer API and related macros for implementors of JSTraceOp, to
 * enumerate all references to traceable things reachable via a property or
 * other strong ref identified for debugging purposes by name or index or
 * a naming callback.
 *
 * See the JSTraceOp typedef.
 */

/*
 * Use the following macros to check if a particular jsval is a traceable
 * thing and to extract the thing and its kind to pass to JS_CallTracer.
 */
static JS_ALWAYS_INLINE JSBool
JSVAL_IS_TRACEABLE(jsval v)
{
    return JSVAL_IS_TRACEABLE_IMPL(JSVAL_TO_IMPL(v));
}

static JS_ALWAYS_INLINE void *
JSVAL_TO_TRACEABLE(jsval v)
{
    return JSVAL_TO_GCTHING(v);
}

static JS_ALWAYS_INLINE JSGCTraceKind
JSVAL_TRACE_KIND(jsval v)
{
    JS_ASSERT(JSVAL_IS_GCTHING(v));
    return (JSGCTraceKind) JSVAL_TRACE_KIND_IMPL(JSVAL_TO_IMPL(v));
}

/*
 * Tracer callback, called for each traceable thing directly referenced by a
 * particular object or runtime structure. It is the callback responsibility
 * to ensure the traversal of the full object graph via calling eventually
 * JS_TraceChildren on the passed thing. In this case the callback must be
 * prepared to deal with cycles in the traversal graph.
 *
 * kind argument is one of JSTRACE_OBJECT, JSTRACE_STRING or a tag denoting
 * internal implementation-specific traversal kind. In the latter case the only
 * operations on thing that the callback can do is to call JS_TraceChildren or
 * JS_GetTraceThingInfo.
 *
 * If eagerlyTraceWeakMaps is true, when we trace a WeakMap visit all
 * of its mappings.  This should be used in cases where the tracer
 * wants to use the existing liveness of entries.
 */
typedef void
(* JSTraceCallback)(JSTracer *trc, void **thingp, JSGCTraceKind kind);

enum WeakMapTraceKind {
    DoNotTraceWeakMaps = 0,
    TraceWeakMapValues = 1,
    TraceWeakMapKeysValues = 2
};

struct JSTracer {
    JSRuntime           *runtime;
    JSTraceCallback     callback;
    JSTraceNamePrinter  debugPrinter;
    const void          *debugPrintArg;
    size_t              debugPrintIndex;
    WeakMapTraceKind    eagerlyTraceWeakMaps;
#ifdef JS_GC_ZEAL
    void                *realLocation;
#endif
};

/*
 * Set debugging information about a reference to a traceable thing to prepare
 * for the following call to JS_CallTracer.
 *
 * When printer is null, arg must be const char * or char * C string naming
 * the reference and index must be either (size_t)-1 indicating that the name
 * alone describes the reference or it must be an index into some array vector
 * that stores the reference.
 *
 * When printer callback is not null, the arg and index arguments are
 * available to the callback as debugPrintArg and debugPrintIndex fields
 * of JSTracer.
 *
 * The storage for name or callback's arguments needs to live only until
 * the following call to JS_CallTracer returns.
 */
# define JS_SET_TRACING_DETAILS(trc, printer, arg, index)                     \
    JS_BEGIN_MACRO                                                            \
        (trc)->debugPrinter = (printer);                                      \
        (trc)->debugPrintArg = (arg);                                         \
        (trc)->debugPrintIndex = (index);                                     \
    JS_END_MACRO

/*
 * Sets the real location for a marked reference, when passing the address
 * directly is not feasable.
 *
 * FIXME: This is currently overcomplicated by our need to nest calls for Values
 * stored as keys in hash tables, but will get simplified once we can rekey
 * in-place.
 */
#ifdef JS_GC_ZEAL
# define JS_SET_TRACING_LOCATION(trc, location)                               \
    JS_BEGIN_MACRO                                                            \
        if (!(trc)->realLocation || !(location))                              \
            (trc)->realLocation = (location);                                 \
    JS_END_MACRO
# define JS_UNSET_TRACING_LOCATION(trc)                                       \
    JS_BEGIN_MACRO                                                            \
        (trc)->realLocation = NULL;                                           \
    JS_END_MACRO
#else
# define JS_SET_TRACING_LOCATION(trc, location)                               \
    JS_BEGIN_MACRO                                                            \
    JS_END_MACRO
# define JS_UNSET_TRACING_LOCATION(trc)                                       \
    JS_BEGIN_MACRO                                                            \
    JS_END_MACRO
#endif


/*
 * Convenience macro to describe the argument of JS_CallTracer using C string
 * and index.
 */
# define JS_SET_TRACING_INDEX(trc, name, index)                               \
    JS_SET_TRACING_DETAILS(trc, NULL, name, index)

/*
 * Convenience macro to describe the argument of JS_CallTracer using C string.
 */
# define JS_SET_TRACING_NAME(trc, name)                                       \
    JS_SET_TRACING_DETAILS(trc, NULL, name, (size_t)-1)

/*
 * The JS_Call*Tracer family of functions traces the given GC thing reference.
 * This performs the tracing action configured on the given JSTracer:
 * typically calling the JSTracer::callback or marking the thing as live.
 *
 * The argument to JS_Call*Tracer is an in-out param: when the function
 * returns, the garbage collector might have moved the GC thing. In this case,
 * the reference passed to JS_Call*Tracer will be updated to the object's new
 * location. Callers of this method are responsible for updating any state
 * that is dependent on the object's address. For example, if the object's
 * address is used as a key in a hashtable, then the object must be removed
 * and re-inserted with the correct hash.
 */
extern JS_PUBLIC_API(void)
JS_CallValueTracer(JSTracer *trc, JS::Value *valuep, const char *name);

extern JS_PUBLIC_API(void)
JS_CallIdTracer(JSTracer *trc, jsid *idp, const char *name);

extern JS_PUBLIC_API(void)
JS_CallObjectTracer(JSTracer *trc, JSObject **objp, const char *name);

extern JS_PUBLIC_API(void)
JS_CallStringTracer(JSTracer *trc, JSString **strp, const char *name);

extern JS_PUBLIC_API(void)
JS_CallScriptTracer(JSTracer *trc, JSScript **scriptp, const char *name);

extern JS_PUBLIC_API(void)
JS_CallHeapValueTracer(JSTracer *trc, JS::Heap<JS::Value> *valuep, const char *name);

extern JS_PUBLIC_API(void)
JS_CallHeapIdTracer(JSTracer *trc, JS::Heap<jsid> *idp, const char *name);

extern JS_PUBLIC_API(void)
JS_CallHeapObjectTracer(JSTracer *trc, JS::Heap<JSObject *> *objp, const char *name);

extern JS_PUBLIC_API(void)
JS_CallHeapStringTracer(JSTracer *trc, JS::Heap<JSString *> *strp, const char *name);

extern JS_PUBLIC_API(void)
JS_CallHeapScriptTracer(JSTracer *trc, JS::Heap<JSScript *> *scriptp, const char *name);

extern JS_PUBLIC_API(void)
JS_CallGenericTracer(JSTracer *trc, void *gcthing, const char *name);

template <typename HashSetEnum>
inline void
JS_CallHashSetObjectTracer(JSTracer *trc, HashSetEnum &e, JSObject *const &key, const char *name)
{
    JSObject *updated = key;
    JS_SET_TRACING_LOCATION(trc, reinterpret_cast<void *>(&const_cast<JSObject *&>(key)));
    JS_CallObjectTracer(trc, &updated, name);
    if (updated != key)
        e.rekeyFront(key, updated);
}

/*
 * The JS_CallMaskedObjectTracer variant traces a JSObject* that is stored
 * with flags embedded in the low bits of the word. The flagMask parameter
 * expects |*objp & flagMask| to yield the flags with the pointer value
 * stripped and |*objp & ~flagMask| to yield a valid GC pointer.
 */
extern JS_PUBLIC_API(void)
JS_CallMaskedObjectTracer(JSTracer *trc, uintptr_t *objp, uintptr_t flagMask, const char *name);

/*
 * API for JSTraceCallback implementations.
 */
extern JS_PUBLIC_API(void)
JS_TracerInit(JSTracer *trc, JSRuntime *rt, JSTraceCallback callback);

extern JS_PUBLIC_API(void)
JS_TraceChildren(JSTracer *trc, void *thing, JSGCTraceKind kind);

extern JS_PUBLIC_API(void)
JS_TraceRuntime(JSTracer *trc);

extern JS_PUBLIC_API(void)
JS_GetTraceThingInfo(char *buf, size_t bufsize, JSTracer *trc,
                     void *thing, JSGCTraceKind kind, JSBool includeDetails);

extern JS_PUBLIC_API(const char *)
JS_GetTraceEdgeName(JSTracer *trc, char *buffer, int bufferSize);

#ifdef DEBUG

/*
 * DEBUG-only method to dump the object graph of heap-allocated things.
 *
 * fp:              file for the dump output.
 * start:           when non-null, dump only things reachable from start
 *                  thing. Otherwise dump all things reachable from the
 *                  runtime roots.
 * startKind:       trace kind of start if start is not null. Must be
 *                  JSTRACE_OBJECT when start is null.
 * thingToFind:     dump only paths in the object graph leading to thingToFind
 *                  when non-null.
 * maxDepth:        the upper bound on the number of edges to descend from the
 *                  graph roots.
 * thingToIgnore:   thing to ignore during the graph traversal when non-null.
 */
extern JS_PUBLIC_API(JSBool)
JS_DumpHeap(JSRuntime *rt, FILE *fp, void* startThing, JSGCTraceKind kind,
            void *thingToFind, size_t maxDepth, void *thingToIgnore);

#endif

/*
 * Garbage collector API.
 */
extern JS_PUBLIC_API(void)
JS_GC(JSRuntime *rt);

extern JS_PUBLIC_API(void)
JS_MaybeGC(JSContext *cx);

extern JS_PUBLIC_API(void)
JS_SetGCCallback(JSRuntime *rt, JSGCCallback cb);

extern JS_PUBLIC_API(void)
JS_SetFinalizeCallback(JSRuntime *rt, JSFinalizeCallback cb);

extern JS_PUBLIC_API(JSBool)
JS_IsGCMarkingTracer(JSTracer *trc);

/*
 * JS_IsAboutToBeFinalized checks if the given object is going to be finalized
 * at the end of the current GC. When called outside of the context of a GC,
 * this function will return false. Typically this function is used on weak
 * references, where the reference should be nulled out or destroyed if the
 * given object is about to be finalized.
 *
 * The argument to JS_IsAboutToBeFinalized is an in-out param: when the
 * function returns false, the object being referenced is still alive, but the
 * garbage collector might have moved it. In this case, the reference passed
 * to JS_IsAboutToBeFinalized will be updated to the object's new location.
 * Callers of this method are responsible for updating any state that is
 * dependent on the object's address. For example, if the object's address is
 * used as a key in a hashtable, then the object must be removed and
 * re-inserted with the correct hash.
 */
extern JS_PUBLIC_API(JSBool)
JS_IsAboutToBeFinalized(JSObject **obj);

typedef enum JSGCParamKey {
    /* Maximum nominal heap before last ditch GC. */
    JSGC_MAX_BYTES          = 0,

    /* Number of JS_malloc bytes before last ditch GC. */
    JSGC_MAX_MALLOC_BYTES   = 1,

    /* Amount of bytes allocated by the GC. */
    JSGC_BYTES = 3,

    /* Number of times when GC was invoked. */
    JSGC_NUMBER = 4,

    /* Max size of the code cache in bytes. */
    JSGC_MAX_CODE_CACHE_BYTES = 5,

    /* Select GC mode. */
    JSGC_MODE = 6,

    /* Number of cached empty GC chunks. */
    JSGC_UNUSED_CHUNKS = 7,

    /* Total number of allocated GC chunks. */
    JSGC_TOTAL_CHUNKS = 8,

    /* Max milliseconds to spend in an incremental GC slice. */
    JSGC_SLICE_TIME_BUDGET = 9,

    /* Maximum size the GC mark stack can grow to. */
    JSGC_MARK_STACK_LIMIT = 10,

    /*
     * GCs less than this far apart in time will be considered 'high-frequency GCs'.
     * See setGCLastBytes in jsgc.cpp.
     */
    JSGC_HIGH_FREQUENCY_TIME_LIMIT = 11,

    /* Start of dynamic heap growth. */
    JSGC_HIGH_FREQUENCY_LOW_LIMIT = 12,

    /* End of dynamic heap growth. */
    JSGC_HIGH_FREQUENCY_HIGH_LIMIT = 13,

    /* Upper bound of heap growth. */
    JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MAX = 14,

    /* Lower bound of heap growth. */
    JSGC_HIGH_FREQUENCY_HEAP_GROWTH_MIN = 15,

    /* Heap growth for low frequency GCs. */
    JSGC_LOW_FREQUENCY_HEAP_GROWTH = 16,

    /*
     * If false, the heap growth factor is fixed at 3. If true, it is determined
     * based on whether GCs are high- or low- frequency.
     */
    JSGC_DYNAMIC_HEAP_GROWTH = 17,

    /* If true, high-frequency GCs will use a longer mark slice. */
    JSGC_DYNAMIC_MARK_SLICE = 18,

    /* Number of megabytes of analysis data to allocate before purging. */
    JSGC_ANALYSIS_PURGE_TRIGGER = 19,

    /* Lower limit after which we limit the heap growth. */
    JSGC_ALLOCATION_THRESHOLD = 20,

    /*
     * We decommit memory lazily. If more than this number of megabytes is
     * available to be decommitted, then JS_MaybeGC will trigger a shrinking GC
     * to decommit it.
     */
    JSGC_DECOMMIT_THRESHOLD = 21
} JSGCParamKey;

typedef enum JSGCMode {
    /* Perform only global GCs. */
    JSGC_MODE_GLOBAL = 0,

    /* Perform per-compartment GCs until too much garbage has accumulated. */
    JSGC_MODE_COMPARTMENT = 1,

    /*
     * Collect in short time slices rather than all at once. Implies
     * JSGC_MODE_COMPARTMENT.
     */
    JSGC_MODE_INCREMENTAL = 2
} JSGCMode;

extern JS_PUBLIC_API(void)
JS_SetGCParameter(JSRuntime *rt, JSGCParamKey key, uint32_t value);

extern JS_PUBLIC_API(uint32_t)
JS_GetGCParameter(JSRuntime *rt, JSGCParamKey key);

extern JS_PUBLIC_API(void)
JS_SetGCParameterForThread(JSContext *cx, JSGCParamKey key, uint32_t value);

extern JS_PUBLIC_API(uint32_t)
JS_GetGCParameterForThread(JSContext *cx, JSGCParamKey key);

/*
 * Create a new JSString whose chars member refers to external memory, i.e.,
 * memory requiring application-specific finalization.
 */
extern JS_PUBLIC_API(JSString *)
JS_NewExternalString(JSContext *cx, const jschar *chars, size_t length,
                     const JSStringFinalizer *fin);

/*
 * Return whether 'str' was created with JS_NewExternalString or
 * JS_NewExternalStringWithClosure.
 */
extern JS_PUBLIC_API(JSBool)
JS_IsExternalString(JSString *str);

/*
 * Return the 'closure' arg passed to JS_NewExternalStringWithClosure or NULL
 * if the external string was created via JS_NewExternalString.
 */
extern JS_PUBLIC_API(const JSStringFinalizer *)
JS_GetExternalStringFinalizer(JSString *str);

/*
 * Set the size of the native stack that should not be exceed. To disable
 * stack size checking pass 0.
 */
extern JS_PUBLIC_API(void)
JS_SetNativeStackQuota(JSRuntime *cx, size_t stackSize);

/************************************************************************/

/*
 * Classes, objects, and properties.
 */
typedef void (*JSClassInternal)();

struct JSClass {
    const char          *name;
    uint32_t            flags;

    /* Mandatory non-null function pointer members. */
    JSPropertyOp        addProperty;
    JSDeletePropertyOp  delProperty;
    JSPropertyOp        getProperty;
    JSStrictPropertyOp  setProperty;
    JSEnumerateOp       enumerate;
    JSResolveOp         resolve;
    JSConvertOp         convert;
    JSFinalizeOp        finalize;

    /* Optionally non-null members start here. */
    JSCheckAccessOp     checkAccess;
    JSNative            call;
    JSHasInstanceOp     hasInstance;
    JSNative            construct;
    JSTraceOp           trace;

    void                *reserved[40];
};

#define JSCLASS_HAS_PRIVATE             (1<<0)  /* objects have private slot */
#define JSCLASS_NEW_ENUMERATE           (1<<1)  /* has JSNewEnumerateOp hook */
#define JSCLASS_NEW_RESOLVE             (1<<2)  /* has JSNewResolveOp hook */
#define JSCLASS_PRIVATE_IS_NSISUPPORTS  (1<<3)  /* private is (nsISupports *) */
#define JSCLASS_IS_DOMJSCLASS           (1<<4)  /* objects are DOM */
#define JSCLASS_IMPLEMENTS_BARRIERS     (1<<5)  /* Correctly implements GC read
                                                   and write barriers */
#define JSCLASS_EMULATES_UNDEFINED      (1<<6)  /* objects of this class act
                                                   like the value undefined,
                                                   in some contexts */
#define JSCLASS_USERBIT1                (1<<7)  /* Reserved for embeddings. */

/*
 * To reserve slots fetched and stored via JS_Get/SetReservedSlot, bitwise-or
 * JSCLASS_HAS_RESERVED_SLOTS(n) into the initializer for JSClass.flags, where
 * n is a constant in [1, 255].  Reserved slots are indexed from 0 to n-1.
 */
#define JSCLASS_RESERVED_SLOTS_SHIFT    8       /* room for 8 flags below */
#define JSCLASS_RESERVED_SLOTS_WIDTH    8       /* and 16 above this field */
#define JSCLASS_RESERVED_SLOTS_MASK     JS_BITMASK(JSCLASS_RESERVED_SLOTS_WIDTH)
#define JSCLASS_HAS_RESERVED_SLOTS(n)   (((n) & JSCLASS_RESERVED_SLOTS_MASK)  \
                                         << JSCLASS_RESERVED_SLOTS_SHIFT)
#define JSCLASS_RESERVED_SLOTS(clasp)   (((clasp)->flags                      \
                                          >> JSCLASS_RESERVED_SLOTS_SHIFT)    \
                                         & JSCLASS_RESERVED_SLOTS_MASK)

#define JSCLASS_HIGH_FLAGS_SHIFT        (JSCLASS_RESERVED_SLOTS_SHIFT +       \
                                         JSCLASS_RESERVED_SLOTS_WIDTH)

#define JSCLASS_IS_ANONYMOUS            (1<<(JSCLASS_HIGH_FLAGS_SHIFT+0))
#define JSCLASS_IS_GLOBAL               (1<<(JSCLASS_HIGH_FLAGS_SHIFT+1))
#define JSCLASS_INTERNAL_FLAG2          (1<<(JSCLASS_HIGH_FLAGS_SHIFT+2))
#define JSCLASS_INTERNAL_FLAG3          (1<<(JSCLASS_HIGH_FLAGS_SHIFT+3))

/* Indicate whether the proto or ctor should be frozen. */
#define JSCLASS_FREEZE_PROTO            (1<<(JSCLASS_HIGH_FLAGS_SHIFT+4))
#define JSCLASS_FREEZE_CTOR             (1<<(JSCLASS_HIGH_FLAGS_SHIFT+5))

/* Reserved for embeddings. */
#define JSCLASS_USERBIT2                (1<<(JSCLASS_HIGH_FLAGS_SHIFT+6))
#define JSCLASS_USERBIT3                (1<<(JSCLASS_HIGH_FLAGS_SHIFT+7))

#define JSCLASS_BACKGROUND_FINALIZE     (1<<(JSCLASS_HIGH_FLAGS_SHIFT+8))

/*
 * Bits 26 through 31 are reserved for the CACHED_PROTO_KEY mechanism, see
 * below.
 */

/* Global flags. */
#define JSGLOBAL_FLAGS_CLEARED          0x1

/*
 * ECMA-262 requires that most constructors used internally create objects
 * with "the original Foo.prototype value" as their [[Prototype]] (__proto__)
 * member initial value.  The "original ... value" verbiage is there because
 * in ECMA-262, global properties naming class objects are read/write and
 * deleteable, for the most part.
 *
 * Implementing this efficiently requires that global objects have classes
 * with the following flags. Failure to use JSCLASS_GLOBAL_FLAGS was
 * prevously allowed, but is now an ES5 violation and thus unsupported.
 */
#define JSCLASS_GLOBAL_SLOT_COUNT      (JSProto_LIMIT * 3 + 25)
#define JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(n)                                    \
    (JSCLASS_IS_GLOBAL | JSCLASS_HAS_RESERVED_SLOTS(JSCLASS_GLOBAL_SLOT_COUNT + (n)))
#define JSCLASS_GLOBAL_FLAGS                                                  \
    JSCLASS_GLOBAL_FLAGS_WITH_SLOTS(0)
#define JSCLASS_HAS_GLOBAL_FLAG_AND_SLOTS(clasp)                              \
  (((clasp)->flags & JSCLASS_IS_GLOBAL)                                       \
   && JSCLASS_RESERVED_SLOTS(clasp) >= JSCLASS_GLOBAL_SLOT_COUNT)

/* Fast access to the original value of each standard class's prototype. */
#define JSCLASS_CACHED_PROTO_SHIFT      (JSCLASS_HIGH_FLAGS_SHIFT + 10)
#define JSCLASS_CACHED_PROTO_WIDTH      6
#define JSCLASS_CACHED_PROTO_MASK       JS_BITMASK(JSCLASS_CACHED_PROTO_WIDTH)
#define JSCLASS_HAS_CACHED_PROTO(key)   (uint32_t(key) << JSCLASS_CACHED_PROTO_SHIFT)
#define JSCLASS_CACHED_PROTO_KEY(clasp) ((JSProtoKey)                         \
                                         (((clasp)->flags                     \
                                           >> JSCLASS_CACHED_PROTO_SHIFT)     \
                                          & JSCLASS_CACHED_PROTO_MASK))

/* Initializer for unused members of statically initialized JSClass structs. */
#define JSCLASS_NO_INTERNAL_MEMBERS     {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
#define JSCLASS_NO_OPTIONAL_MEMBERS     0,0,0,0,0,JSCLASS_NO_INTERNAL_MEMBERS

extern JS_PUBLIC_API(int)
JS_IdArrayLength(JSContext *cx, JSIdArray *ida);

extern JS_PUBLIC_API(jsid)
JS_IdArrayGet(JSContext *cx, JSIdArray *ida, int index);

extern JS_PUBLIC_API(void)
JS_DestroyIdArray(JSContext *cx, JSIdArray *ida);

namespace JS {

class AutoIdArray : private AutoGCRooter
{
  public:
    AutoIdArray(JSContext *cx, JSIdArray *ida
                MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : AutoGCRooter(cx, IDARRAY), context(cx), idArray(ida)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }
    ~AutoIdArray() {
        if (idArray)
            JS_DestroyIdArray(context, idArray);
    }
    bool operator!() {
        return !idArray;
    }
    jsid operator[](size_t i) const {
        JS_ASSERT(idArray);
        JS_ASSERT(i < length());
        return JS_IdArrayGet(context, idArray, i);
    }
    size_t length() const {
        return JS_IdArrayLength(context, idArray);
    }

    friend void AutoGCRooter::trace(JSTracer *trc);

    JSIdArray *steal() {
        JSIdArray *copy = idArray;
        idArray = NULL;
        return copy;
    }

  protected:
    inline void trace(JSTracer *trc);

  private:
    JSContext *context;
    JSIdArray *idArray;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    /* No copy or assignment semantics. */
    AutoIdArray(AutoIdArray &ida) MOZ_DELETE;
    void operator=(AutoIdArray &ida) MOZ_DELETE;
};

} /* namespace JS */

extern JS_PUBLIC_API(JSBool)
JS_ValueToId(JSContext *cx, jsval v, jsid *idp);

extern JS_PUBLIC_API(JSBool)
JS_IdToValue(JSContext *cx, jsid id, jsval *vp);

/*
 * JSNewResolveOp flag bits.
 */
#define JSRESOLVE_ASSIGNING     0x01    /* resolve on the left of assignment */

/*
 * Invoke the [[DefaultValue]] hook (see ES5 8.6.2) with the provided hint on
 * the specified object, computing a primitive default value for the object.
 * The hint must be JSTYPE_STRING, JSTYPE_NUMBER, or JSTYPE_VOID (no hint).  On
 * success the resulting value is stored in *vp.
 */
extern JS_PUBLIC_API(JSBool)
JS_DefaultValue(JSContext *cx, JSObject *obj, JSType hint, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_PropertyStub(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
                JS::MutableHandle<JS::Value> vp);

extern JS_PUBLIC_API(JSBool)
JS_StrictPropertyStub(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id, JSBool strict,
                      JS::MutableHandle<JS::Value> vp);

extern JS_PUBLIC_API(JSBool)
JS_DeletePropertyStub(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id,
                      JSBool *succeeded);

extern JS_PUBLIC_API(JSBool)
JS_EnumerateStub(JSContext *cx, JS::Handle<JSObject*> obj);

extern JS_PUBLIC_API(JSBool)
JS_ResolveStub(JSContext *cx, JS::Handle<JSObject*> obj, JS::Handle<jsid> id);

extern JS_PUBLIC_API(JSBool)
JS_ConvertStub(JSContext *cx, JS::Handle<JSObject*> obj, JSType type,
               JS::MutableHandle<JS::Value> vp);

struct JSConstDoubleSpec {
    double          dval;
    const char      *name;
    uint8_t         flags;
    uint8_t         spare[3];
};

typedef struct JSJitInfo JSJitInfo;

/*
 * Wrappers to replace {Strict,}PropertyOp for JSPropertySpecs. This will allow
 * us to pass one JSJitInfo per function with the property spec, without
 * additional field overhead.
 */
typedef struct JSStrictPropertyOpWrapper {
    JSStrictPropertyOp  op;
    const JSJitInfo     *info;
} JSStrictPropertyOpWrapper;

typedef struct JSPropertyOpWrapper {
    JSPropertyOp        op;
    const JSJitInfo     *info;
} JSPropertyOpWrapper;

/*
 * Wrapper to do as above, but for JSNatives for JSFunctionSpecs.
 */
typedef struct JSNativeWrapper {
    JSNative        op;
    const JSJitInfo *info;
} JSNativeWrapper;

/*
 * Macro static initializers which make it easy to pass no JSJitInfo as part of a
 * JSPropertySpec or JSFunctionSpec.
 */
#define JSOP_WRAPPER(op) {op, NULL}
#define JSOP_NULLWRAPPER JSOP_WRAPPER(NULL)

/*
 * To define an array element rather than a named property member, cast the
 * element's index to (const char *) and initialize name with it, and set the
 * JSPROP_INDEX bit in flags.
 */
struct JSPropertySpec {
    const char                  *name;
    int8_t                      tinyid;
    uint8_t                     flags;
    JSPropertyOpWrapper         getter;
    JSStrictPropertyOpWrapper   setter;
};

namespace JS {
namespace detail {

/* NEVER DEFINED, DON'T USE.  For use by JS_CAST_NATIVE_TO only. */
inline int CheckIsNative(JSNative native);

} // namespace detail
} // namespace JS

#define JS_CAST_NATIVE_TO(v, To) \
  (static_cast<void>(sizeof(JS::detail::CheckIsNative(v))), \
   reinterpret_cast<To>(v))

/*
 * JSPropertySpec uses JSAPI JSPropertyOp and JSStrictPropertyOp in function
 * signatures, but with JSPROP_NATIVE_ACCESSORS the actual values must be
 * JSNatives. To avoid widespread casting, have JS_PSG and JS_PSGS perform
 * type-safe casts.
 */
#define JS_PSG(name, getter, flags) \
    {name, 0, (flags) | JSPROP_SHARED | JSPROP_NATIVE_ACCESSORS, \
     JSOP_WRAPPER(JS_CAST_NATIVE_TO(getter, JSPropertyOp)), \
     JSOP_NULLWRAPPER}
#define JS_PSGS(name, getter, setter, flags) \
    {name, 0, (flags) | JSPROP_SHARED | JSPROP_NATIVE_ACCESSORS, \
     JSOP_WRAPPER(JS_CAST_NATIVE_TO(getter, JSPropertyOp)), \
     JSOP_WRAPPER(JS_CAST_NATIVE_TO(setter, JSStrictPropertyOp))}
#define JS_PS_END {0, 0, 0, JSOP_NULLWRAPPER, JSOP_NULLWRAPPER}

/*
 * To define a native function, set call to a JSNativeWrapper. To define a
 * self-hosted function, set selfHostedName to the name of a function
 * compiled during JSRuntime::initSelfHosting.
 */
struct JSFunctionSpec {
    const char      *name;
    JSNativeWrapper call;
    uint16_t        nargs;
    uint16_t        flags;
    const char      *selfHostedName;
};

/*
 * Terminating sentinel initializer to put at the end of a JSFunctionSpec array
 * that's passed to JS_DefineFunctions or JS_InitClass.
 */
#define JS_FS_END JS_FS(NULL,NULL,0,0)

/*
 * Initializer macros for a JSFunctionSpec array element. JS_FN (whose name
 * pays homage to the old JSNative/JSFastNative split) simply adds the flag
 * JSFUN_STUB_GSOPS. JS_FNINFO allows the simple adding of JSJitInfos.
 */
#define JS_FS(name,call,nargs,flags)                                          \
    {name, JSOP_WRAPPER(call), nargs, flags}
#define JS_FN(name,call,nargs,flags)                                          \
    {name, JSOP_WRAPPER(call), nargs, (flags) | JSFUN_STUB_GSOPS}
#define JS_FNINFO(name,call,info,nargs,flags)                                 \
    {name,{call,info},nargs,flags}

extern JS_PUBLIC_API(JSObject *)
JS_InitClass(JSContext *cx, JSObject *obj, JSObject *parent_proto,
             JSClass *clasp, JSNative constructor, unsigned nargs,
             const JSPropertySpec *ps, const JSFunctionSpec *fs,
             const JSPropertySpec *static_ps, const JSFunctionSpec *static_fs);

/*
 * Set up ctor.prototype = proto and proto.constructor = ctor with the
 * right property flags.
 */
extern JS_PUBLIC_API(JSBool)
JS_LinkConstructorAndPrototype(JSContext *cx, JSObject *ctor, JSObject *proto);

extern JS_PUBLIC_API(JSClass *)
JS_GetClass(JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_InstanceOf(JSContext *cx, JSObject *obj, JSClass *clasp, jsval *argv);

extern JS_PUBLIC_API(JSBool)
JS_HasInstance(JSContext *cx, JSObject *obj, jsval v, JSBool *bp);

extern JS_PUBLIC_API(void *)
JS_GetPrivate(JSObject *obj);

extern JS_PUBLIC_API(void)
JS_SetPrivate(JSObject *obj, void *data);

extern JS_PUBLIC_API(void *)
JS_GetInstancePrivate(JSContext *cx, JSObject *obj, JSClass *clasp,
                      jsval *argv);

extern JS_PUBLIC_API(JSBool)
JS_GetPrototype(JSContext *cx, JSObject *obj, JSObject **protop);

extern JS_PUBLIC_API(JSBool)
JS_SetPrototype(JSContext *cx, JSObject *obj, JSObject *proto);

extern JS_PUBLIC_API(JSObject *)
JS_GetParent(JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_SetParent(JSContext *cx, JSObject *obj, JSObject *parent);

extern JS_PUBLIC_API(JSObject *)
JS_GetConstructor(JSContext *cx, JSObject *proto);

/*
 * Get a unique identifier for obj, good for the lifetime of obj (even if it
 * is moved by a copying GC).  Return false on failure (likely out of memory),
 * and true with *idp containing the unique id on success.
 */
extern JS_PUBLIC_API(JSBool)
JS_GetObjectId(JSContext *cx, JSObject *obj, jsid *idp);

namespace JS {

enum {
    FreshZone,
    SystemZone,
    SpecificZones
};

typedef uintptr_t ZoneSpecifier;

inline ZoneSpecifier
SameZoneAs(JSObject *obj)
{
    JS_ASSERT(uintptr_t(obj) > SpecificZones);
    return ZoneSpecifier(obj);
}

struct JS_PUBLIC_API(CompartmentOptions) {
    ZoneSpecifier zoneSpec;
    bool hasVersion;
    JSVersion version;

    explicit CompartmentOptions() : zoneSpec(JS::FreshZone)
                                  , hasVersion(false)
                                  , version(JSVERSION_UNKNOWN)
    {}

    CompartmentOptions &setZone(ZoneSpecifier spec) { zoneSpec = spec; return *this; }
    CompartmentOptions &setVersion(JSVersion version_) { hasVersion = true; version = version_; return *this; }
};

} /* namespace JS */

extern JS_PUBLIC_API(JSObject *)
JS_NewGlobalObject(JSContext *cx, JSClass *clasp, JSPrincipals *principals,
                   const JS::CompartmentOptions &options = JS::CompartmentOptions());

extern JS_PUBLIC_API(JSObject *)
JS_NewObject(JSContext *cx, JSClass *clasp, JSObject *proto, JSObject *parent);

/* Queries the [[Extensible]] property of the object. */
extern JS_PUBLIC_API(JSBool)
JS_IsExtensible(JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_IsNative(JSObject *obj);

extern JS_PUBLIC_API(JSRuntime *)
JS_GetObjectRuntime(JSObject *obj);

/*
 * Unlike JS_NewObject, JS_NewObjectWithGivenProto does not compute a default
 * proto if proto's actual parameter value is null.
 */
extern JS_PUBLIC_API(JSObject *)
JS_NewObjectWithGivenProto(JSContext *cx, JSClass *clasp, JSObject *proto,
                           JSObject *parent);

/*
 * Freeze obj, and all objects it refers to, recursively. This will not recurse
 * through non-extensible objects, on the assumption that those are already
 * deep-frozen.
 */
extern JS_PUBLIC_API(JSBool)
JS_DeepFreezeObject(JSContext *cx, JSObject *obj);

/*
 * Freezes an object; see ES5's Object.freeze(obj) method.
 */
extern JS_PUBLIC_API(JSBool)
JS_FreezeObject(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_PreventExtensions(JSContext *cx, JS::HandleObject obj);

extern JS_PUBLIC_API(JSObject *)
JS_New(JSContext *cx, JSObject *ctor, unsigned argc, jsval *argv);

extern JS_PUBLIC_API(JSObject *)
JS_DefineObject(JSContext *cx, JSObject *obj, const char *name, JSClass *clasp,
                JSObject *proto, unsigned attrs);

extern JS_PUBLIC_API(JSBool)
JS_DefineConstDoubles(JSContext *cx, JSObject *obj, const JSConstDoubleSpec *cds);

extern JS_PUBLIC_API(JSBool)
JS_DefineProperties(JSContext *cx, JSObject *obj, const JSPropertySpec *ps);

extern JS_PUBLIC_API(JSBool)
JS_DefineProperty(JSContext *cx, JSObject *obj, const char *name, jsval value,
                  JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs);

extern JS_PUBLIC_API(JSBool)
JS_DefinePropertyById(JSContext *cx, JSObject *obj, jsid id, jsval value,
                      JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs);

extern JS_PUBLIC_API(JSBool)
JS_DefineOwnProperty(JSContext *cx, JSObject *obj, jsid id, jsval descriptor, JSBool *bp);

/*
 * Determine the attributes (JSPROP_* flags) of a property on a given object.
 *
 * If the object does not have a property by that name, *foundp will be
 * JS_FALSE and the value of *attrsp is undefined.
 */
extern JS_PUBLIC_API(JSBool)
JS_GetPropertyAttributes(JSContext *cx, JSObject *obj, const char *name,
                         unsigned *attrsp, JSBool *foundp);

/*
 * The same, but if the property is native, return its getter and setter via
 * *getterp and *setterp, respectively (and only if the out parameter pointer
 * is not null).
 */
extern JS_PUBLIC_API(JSBool)
JS_GetPropertyAttrsGetterAndSetter(JSContext *cx, JSObject *obj,
                                   const char *name,
                                   unsigned *attrsp, JSBool *foundp,
                                   JSPropertyOp *getterp,
                                   JSStrictPropertyOp *setterp);

extern JS_PUBLIC_API(JSBool)
JS_GetPropertyAttrsGetterAndSetterById(JSContext *cx, JSObject *obj,
                                       jsid id,
                                       unsigned *attrsp, JSBool *foundp,
                                       JSPropertyOp *getterp,
                                       JSStrictPropertyOp *setterp);

/*
 * Set the attributes of a property on a given object.
 *
 * If the object does not have a property by that name, *foundp will be
 * JS_FALSE and nothing will be altered.
 */
extern JS_PUBLIC_API(JSBool)
JS_SetPropertyAttributes(JSContext *cx, JSObject *obj, const char *name,
                         unsigned attrs, JSBool *foundp);

extern JS_PUBLIC_API(JSBool)
JS_DefinePropertyWithTinyId(JSContext *cx, JSObject *obj, const char *name,
                            int8_t tinyid, jsval value,
                            JSPropertyOp getter, JSStrictPropertyOp setter,
                            unsigned attrs);

extern JS_PUBLIC_API(JSBool)
JS_AlreadyHasOwnProperty(JSContext *cx, JSObject *obj, const char *name,
                         JSBool *foundp);

extern JS_PUBLIC_API(JSBool)
JS_AlreadyHasOwnPropertyById(JSContext *cx, JSObject *obj, jsid id,
                             JSBool *foundp);

extern JS_PUBLIC_API(JSBool)
JS_HasProperty(JSContext *cx, JSObject *obj, const char *name, JSBool *foundp);

extern JS_PUBLIC_API(JSBool)
JS_HasPropertyById(JSContext *cx, JSObject *obj, jsid id, JSBool *foundp);

extern JS_PUBLIC_API(JSBool)
JS_LookupProperty(JSContext *cx, JSObject *obj, const char *name, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_LookupPropertyById(JSContext *cx, JSObject *obj, jsid id, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_LookupPropertyWithFlags(JSContext *cx, JSObject *obj, const char *name,
                           unsigned flags, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_LookupPropertyWithFlagsById(JSContext *cx, JSObject *obj, jsid id,
                               unsigned flags, JSObject **objp, jsval *vp);

struct JSPropertyDescriptor {
    JSObject           *obj;
    unsigned           attrs;
    unsigned           shortid;
    JSPropertyOp       getter;
    JSStrictPropertyOp setter;
    JS::Value          value;

    JSPropertyDescriptor() : obj(NULL), attrs(0), shortid(0), getter(NULL),
                             setter(NULL), value(JSVAL_VOID)
    {}

    void trace(JSTracer *trc);
};

namespace JS {

template <typename Outer>
class PropertyDescriptorOperations
{
    const JSPropertyDescriptor * desc() const { return static_cast<const Outer*>(this)->extract(); }
    JSPropertyDescriptor * desc() { return static_cast<Outer*>(this)->extract(); }

  public:
    bool isEnumerable() const { return desc()->attrs & JSPROP_ENUMERATE; }
    bool isReadonly() const { return desc()->attrs & JSPROP_READONLY; }
    bool isPermanent() const { return desc()->attrs & JSPROP_PERMANENT; }
    bool hasNativeAccessors() const { return desc()->attrs & JSPROP_NATIVE_ACCESSORS; }
    bool hasGetterObject() const { return desc()->attrs & JSPROP_GETTER; }
    bool hasSetterObject() const { return desc()->attrs & JSPROP_SETTER; }
    bool isShared() const { return desc()->attrs & JSPROP_SHARED; }
    bool isIndex() const { return desc()->attrs & JSPROP_INDEX; }
    bool hasShortId() const { return desc()->attrs & JSPROP_SHORTID; }
    bool hasAttributes(unsigned attrs) const { return desc()->attrs & attrs; }

    JS::MutableHandleObject object() { return JS::MutableHandleObject::fromMarkedLocation(&desc()->obj); }
    unsigned attributes() const { return desc()->attrs; }
    unsigned shortid() const {
        MOZ_ASSERT(hasShortId());
        return desc()->shortid;
    }
    JSPropertyOp getter() const { return desc()->getter; }
    JSStrictPropertyOp setter() const { return desc()->setter; }
    JS::HandleObject getterObject() const {
        MOZ_ASSERT(hasGetterObject());
        return JS::HandleObject::fromMarkedLocation(reinterpret_cast<JSObject *const *>(&desc()->getter));
    }
    JS::HandleObject setterObject() const {
        MOZ_ASSERT(hasSetterObject());
        return JS::HandleObject::fromMarkedLocation(reinterpret_cast<JSObject *const *>(&desc()->setter));
    }
    JS::MutableHandleValue value() { return JS::MutableHandleValue::fromMarkedLocation(&desc()->value); }

    void setAttributes(unsigned attrs) { desc()->attrs = attrs; }
    void setShortId(unsigned id) { desc()->shortid = id; }
    void setGetter(JSPropertyOp op) { desc()->getter = op; }
    void setSetter(JSStrictPropertyOp op) { desc()->setter = op; }
    void setGetterObject(JSObject *obj) { desc()->getter = reinterpret_cast<JSPropertyOp>(obj); }
    void setSetterObject(JSObject *obj) { desc()->setter = reinterpret_cast<JSStrictPropertyOp>(obj); }
};

} /* namespace JS */

namespace js {

template <>
struct GCMethods<JSPropertyDescriptor> {
    static JSPropertyDescriptor initial() { return JSPropertyDescriptor(); }
    static ThingRootKind kind() { return THING_ROOT_PROPERTY_DESCRIPTOR; }
    static bool poisoned(const JSPropertyDescriptor &desc) {
        return (desc.obj && JS::IsPoisonedPtr(desc.obj)) ||
               (desc.attrs & JSPROP_GETTER && desc.getter && JS::IsPoisonedPtr(desc.getter)) ||
               (desc.attrs & JSPROP_SETTER && desc.setter && JS::IsPoisonedPtr(desc.setter)) ||
               (desc.value.isGCThing() && JS::IsPoisonedPtr(desc.value.toGCThing()));
    }
};

template <>
class RootedBase<JSPropertyDescriptor>
  : public JS::PropertyDescriptorOperations<JS::Rooted<JSPropertyDescriptor> >
{
    friend class JS::PropertyDescriptorOperations<JS::Rooted<JSPropertyDescriptor> >;
    const JSPropertyDescriptor *extract() const {
        return static_cast<const JS::Rooted<JSPropertyDescriptor>*>(this)->address();
    }
    JSPropertyDescriptor *extract() {
        return static_cast<JS::Rooted<JSPropertyDescriptor>*>(this)->address();
    }
};

template <>
class HandleBase<JSPropertyDescriptor>
  : public JS::PropertyDescriptorOperations<JS::Handle<JSPropertyDescriptor> >
{
    friend class JS::PropertyDescriptorOperations<JS::Handle<JSPropertyDescriptor> >;
    const JSPropertyDescriptor *extract() const {
        return static_cast<const JS::Handle<JSPropertyDescriptor>*>(this)->address();
    }
  public:
    JS::HandleValue value() const { return JS::HandleValue::fromMarkedLocation(&extract()->value); }
    JS::HandleObject obj() const { return JS::HandleObject::fromMarkedLocation(&extract()->obj); }
};

template <>
class MutableHandleBase<JSPropertyDescriptor>
  : public JS::PropertyDescriptorOperations<JS::MutableHandle<JSPropertyDescriptor> >
{
    friend class JS::PropertyDescriptorOperations<JS::MutableHandle<JSPropertyDescriptor> >;
    const JSPropertyDescriptor *extract() const {
        return static_cast<const JS::MutableHandle<JSPropertyDescriptor>*>(this)->address();
    }
    JSPropertyDescriptor *extract() {
        return static_cast<JS::MutableHandle<JSPropertyDescriptor>*>(this)->address();
    }
};

} /* namespace js */

/*
 * Like JS_GetPropertyAttrsGetterAndSetterById but will return a property on
 * an object on the prototype chain (returned in objp). If data->obj is null,
 * then this property was not found on the prototype chain.
 */
extern JS_PUBLIC_API(JSBool)
JS_GetPropertyDescriptorById(JSContext *cx, JSObject *obj, jsid id, unsigned flags,
                             JSPropertyDescriptor *desc);

extern JS_PUBLIC_API(JSBool)
JS_GetOwnPropertyDescriptor(JSContext *cx, JSObject *obj, jsid id, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_GetProperty(JSContext *cx, JSObject *obj, const char *name, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_GetPropertyDefault(JSContext *cx, JSObject *obj, const char *name, jsval def, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_GetPropertyById(JSContext *cx, JSObject *obj, jsid id, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_GetPropertyByIdDefault(JSContext *cx, JSObject *obj, jsid id, jsval def, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_ForwardGetPropertyTo(JSContext *cx, JSObject *obj, jsid id, JSObject *onBehalfOf, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_SetProperty(JSContext *cx, JSObject *obj, const char *name, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_SetPropertyById(JSContext *cx, JSObject *obj, jsid id, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_DeleteProperty(JSContext *cx, JSObject *obj, const char *name);

extern JS_PUBLIC_API(JSBool)
JS_DeleteProperty2(JSContext *cx, JSObject *obj, const char *name,
                   jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_DeletePropertyById(JSContext *cx, JSObject *obj, jsid id);

extern JS_PUBLIC_API(JSBool)
JS_DeletePropertyById2(JSContext *cx, JSObject *obj, jsid id, jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_DefineUCProperty(JSContext *cx, JSObject *obj,
                    const jschar *name, size_t namelen, jsval value,
                    JSPropertyOp getter, JSStrictPropertyOp setter,
                    unsigned attrs);

/*
 * Determine the attributes (JSPROP_* flags) of a property on a given object.
 *
 * If the object does not have a property by that name, *foundp will be
 * JS_FALSE and the value of *attrsp is undefined.
 */
extern JS_PUBLIC_API(JSBool)
JS_GetUCPropertyAttributes(JSContext *cx, JSObject *obj,
                           const jschar *name, size_t namelen,
                           unsigned *attrsp, JSBool *foundp);

/*
 * The same, but if the property is native, return its getter and setter via
 * *getterp and *setterp, respectively (and only if the out parameter pointer
 * is not null).
 */
extern JS_PUBLIC_API(JSBool)
JS_GetUCPropertyAttrsGetterAndSetter(JSContext *cx, JSObject *obj,
                                     const jschar *name, size_t namelen,
                                     unsigned *attrsp, JSBool *foundp,
                                     JSPropertyOp *getterp,
                                     JSStrictPropertyOp *setterp);

/*
 * Set the attributes of a property on a given object.
 *
 * If the object does not have a property by that name, *foundp will be
 * JS_FALSE and nothing will be altered.
 */
extern JS_PUBLIC_API(JSBool)
JS_SetUCPropertyAttributes(JSContext *cx, JSObject *obj,
                           const jschar *name, size_t namelen,
                           unsigned attrs, JSBool *foundp);


extern JS_PUBLIC_API(JSBool)
JS_DefineUCPropertyWithTinyId(JSContext *cx, JSObject *obj,
                              const jschar *name, size_t namelen,
                              int8_t tinyid, jsval value,
                              JSPropertyOp getter, JSStrictPropertyOp setter,
                              unsigned attrs);

extern JS_PUBLIC_API(JSBool)
JS_AlreadyHasOwnUCProperty(JSContext *cx, JSObject *obj, const jschar *name,
                           size_t namelen, JSBool *foundp);

extern JS_PUBLIC_API(JSBool)
JS_HasUCProperty(JSContext *cx, JSObject *obj,
                 const jschar *name, size_t namelen,
                 JSBool *vp);

extern JS_PUBLIC_API(JSBool)
JS_LookupUCProperty(JSContext *cx, JSObject *obj,
                    const jschar *name, size_t namelen,
                    jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_GetUCProperty(JSContext *cx, JSObject *obj,
                 const jschar *name, size_t namelen,
                 jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_SetUCProperty(JSContext *cx, JSObject *obj,
                 const jschar *name, size_t namelen,
                 jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_DeleteUCProperty2(JSContext *cx, JSObject *obj,
                     const jschar *name, size_t namelen,
                     jsval *rval);

extern JS_PUBLIC_API(JSObject *)
JS_NewArrayObject(JSContext *cx, int length, jsval *vector);

extern JS_PUBLIC_API(JSBool)
JS_IsArrayObject(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_GetArrayLength(JSContext *cx, JSObject *obj, uint32_t *lengthp);

extern JS_PUBLIC_API(JSBool)
JS_SetArrayLength(JSContext *cx, JSObject *obj, uint32_t length);

extern JS_PUBLIC_API(JSBool)
JS_DefineElement(JSContext *cx, JSObject *obj, uint32_t index, jsval value,
                 JSPropertyOp getter, JSStrictPropertyOp setter, unsigned attrs);

extern JS_PUBLIC_API(JSBool)
JS_AlreadyHasOwnElement(JSContext *cx, JSObject *obj, uint32_t index, JSBool *foundp);

extern JS_PUBLIC_API(JSBool)
JS_HasElement(JSContext *cx, JSObject *obj, uint32_t index, JSBool *foundp);

extern JS_PUBLIC_API(JSBool)
JS_LookupElement(JSContext *cx, JSObject *obj, uint32_t index, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_GetElement(JSContext *cx, JSObject *obj, uint32_t index, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_ForwardGetElementTo(JSContext *cx, JSObject *obj, uint32_t index, JSObject *onBehalfOf,
                       jsval *vp);

/*
 * Get the property with name given by |index|, if it has one.  If
 * not, |*present| will be set to false and the value of |vp| must not
 * be relied on.
 */
extern JS_PUBLIC_API(JSBool)
JS_GetElementIfPresent(JSContext *cx, JSObject *obj, uint32_t index, JSObject *onBehalfOf,
                       jsval *vp, JSBool* present);

extern JS_PUBLIC_API(JSBool)
JS_SetElement(JSContext *cx, JSObject *obj, uint32_t index, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_DeleteElement(JSContext *cx, JSObject *obj, uint32_t index);

extern JS_PUBLIC_API(JSBool)
JS_DeleteElement2(JSContext *cx, JSObject *obj, uint32_t index, jsval *rval);

/*
 * Remove all configurable properties from the given (non-global) object and
 * assign undefined to all writable data properties.
 */
JS_PUBLIC_API(void)
JS_ClearNonGlobalObject(JSContext *cx, JSObject *objArg);

/*
 * Assign 'undefined' to all of the object's non-reserved slots. Note: this is
 * done for all slots, regardless of the associated property descriptor.
 */
JS_PUBLIC_API(void)
JS_SetAllNonReservedSlotsToUndefined(JSContext *cx, JSObject *objArg);

/*
 * Create a new array buffer with the given contents, which must have been
 * returned by JS_AllocateArrayBufferContents or JS_StealArrayBufferContents.
 * The new array buffer takes ownership. After calling this function, do not
 * free |contents| or use |contents| from another thread.
 */
extern JS_PUBLIC_API(JSObject *)
JS_NewArrayBufferWithContents(JSContext *cx, void *contents);

/*
 * Steal the contents of the given array buffer. The array buffer has its
 * length set to 0 and its contents array cleared. The caller takes ownership
 * of |*contents| and must free it or transfer ownership via
 * JS_NewArrayBufferWithContents when done using it.
 * To free |*contents|, call free().
 * A pointer to the buffer's data is returned in |*data|. This pointer can
 * be used until |*contents| is freed or has its ownership transferred.
 */
extern JS_PUBLIC_API(JSBool)
JS_StealArrayBufferContents(JSContext *cx, JSObject *obj, void **contents,
                            uint8_t **data);

/*
 * Allocate memory that may be eventually passed to
 * JS_NewArrayBufferWithContents. |nbytes| is the number of payload bytes
 * required. The pointer to pass to JS_NewArrayBufferWithContents is returned
 * in |contents|. The pointer to the |nbytes| of usable memory is returned in
 * |data|. (*|contents| will contain a header before |data|.) The only legal
 * operations on *|contents| is to free it, or pass it to
 * JS_NewArrayBufferWithContents or JS_ReallocateArrayBufferContents.
 */
extern JS_PUBLIC_API(JSBool)
JS_AllocateArrayBufferContents(JSContext *cx, uint32_t nbytes, void **contents, uint8_t **data);

/*
 * Reallocate memory allocated by JS_AllocateArrayBufferContents, growing or shrinking it
 * as appropriate.  The new data pointer will be returned in data.  If *contents is NULL,
 * behaves like JS_AllocateArrayBufferContents.
 */
extern JS_PUBLIC_API(JSBool)
JS_ReallocateArrayBufferContents(JSContext *cx, uint32_t nbytes, void **contents, uint8_t **data);

extern JS_PUBLIC_API(JSIdArray *)
JS_Enumerate(JSContext *cx, JSObject *obj);

/*
 * Create an object to iterate over enumerable properties of obj, in arbitrary
 * property definition order.  NB: This differs from longstanding for..in loop
 * order, which uses order of property definition in obj.
 */
extern JS_PUBLIC_API(JSObject *)
JS_NewPropertyIterator(JSContext *cx, JSObject *obj);

/*
 * Return true on success with *idp containing the id of the next enumerable
 * property to visit using iterobj, or JSID_IS_VOID if there is no such property
 * left to visit.  Return false on error.
 */
extern JS_PUBLIC_API(JSBool)
JS_NextProperty(JSContext *cx, JSObject *iterobj, jsid *idp);

/*
 * A JSNative that creates and returns a new iterator that iterates over the
 * elements of |this|, up to |this.length|, in index order. This can be used to
 * make any array-like object iterable. Just give the object an obj.iterator()
 * method using this JSNative as the implementation.
 */
extern JS_PUBLIC_API(JSBool)
JS_ArrayIterator(JSContext *cx, unsigned argc, jsval *vp);

extern JS_PUBLIC_API(JSBool)
JS_CheckAccess(JSContext *cx, JSObject *obj, jsid id, JSAccessMode mode,
               jsval *vp, unsigned *attrsp);

extern JS_PUBLIC_API(jsval)
JS_GetReservedSlot(JSObject *obj, uint32_t index);

extern JS_PUBLIC_API(void)
JS_SetReservedSlot(JSObject *obj, uint32_t index, jsval v);

/************************************************************************/

/*
 * Security protocol.
 */
struct JSPrincipals {
    /* Don't call "destroy"; use reference counting macros below. */
    int refcount;

#ifdef DEBUG
    /* A helper to facilitate principals debugging. */
    uint32_t    debugToken;
#endif

    void setDebugToken(uint32_t token) {
# ifdef DEBUG
        debugToken = token;
# endif
    }

    /*
     * This is not defined by the JS engine but should be provided by the
     * embedding.
     */
    JS_PUBLIC_API(void) dump();
};

extern JS_PUBLIC_API(void)
JS_HoldPrincipals(JSPrincipals *principals);

extern JS_PUBLIC_API(void)
JS_DropPrincipals(JSRuntime *rt, JSPrincipals *principals);

struct JSSecurityCallbacks {
    JSCheckAccessOp            checkObjectAccess;
    JSCSPEvalChecker           contentSecurityPolicyAllows;
    JSXSSFilterChecker         xssFilterAllows;
};

extern JS_PUBLIC_API(void)
JS_SetSecurityCallbacks(JSRuntime *rt, const JSSecurityCallbacks *callbacks);

extern JS_PUBLIC_API(const JSSecurityCallbacks *)
JS_GetSecurityCallbacks(JSRuntime *rt);

/*
 * Code running with "trusted" principals will be given a deeper stack
 * allocation than ordinary scripts. This allows trusted script to run after
 * untrusted script has exhausted the stack. This function sets the
 * runtime-wide trusted principal.
 *
 * This principals is not held (via JS_HoldPrincipals/JS_DropPrincipals) since
 * there is no available JSContext. Instead, the caller must ensure that the
 * given principals stays valid for as long as 'rt' may point to it. If the
 * principals would be destroyed before 'rt', JS_SetTrustedPrincipals must be
 * called again, passing NULL for 'prin'.
 */
extern JS_PUBLIC_API(void)
JS_SetTrustedPrincipals(JSRuntime *rt, JSPrincipals *prin);

/*
 * Initialize the callback that is called to destroy JSPrincipals instance
 * when its reference counter drops to zero. The initialization can be done
 * only once per JS runtime.
 */
extern JS_PUBLIC_API(void)
JS_InitDestroyPrincipalsCallback(JSRuntime *rt, JSDestroyPrincipalsOp destroyPrincipals);

/************************************************************************/

/*
 * Functions and scripts.
 */
extern JS_PUBLIC_API(JSFunction *)
JS_NewFunction(JSContext *cx, JSNative call, unsigned nargs, unsigned flags,
               JSObject *parent, const char *name);

/*
 * Create the function with the name given by the id. JSID_IS_STRING(id) must
 * be true.
 */
extern JS_PUBLIC_API(JSFunction *)
JS_NewFunctionById(JSContext *cx, JSNative call, unsigned nargs, unsigned flags,
                   JSObject *parent, jsid id);

extern JS_PUBLIC_API(JSObject *)
JS_GetFunctionObject(JSFunction *fun);

/*
 * Return the function's identifier as a JSString, or null if fun is unnamed.
 * The returned string lives as long as fun, so you don't need to root a saved
 * reference to it if fun is well-connected or rooted, and provided you bound
 * the use of the saved reference by fun's lifetime.
 */
extern JS_PUBLIC_API(JSString *)
JS_GetFunctionId(JSFunction *fun);

/*
 * Return a function's display name. This is the defined name if one was given
 * where the function was defined, or it could be an inferred name by the JS
 * engine in the case that the function was defined to be anonymous. This can
 * still return NULL if a useful display name could not be inferred. The same
 * restrictions on rooting as those in JS_GetFunctionId apply.
 */
extern JS_PUBLIC_API(JSString *)
JS_GetFunctionDisplayId(JSFunction *fun);

/*
 * Return the arity (length) of fun.
 */
extern JS_PUBLIC_API(uint16_t)
JS_GetFunctionArity(JSFunction *fun);

/*
 * Infallible predicate to test whether obj is a function object (faster than
 * comparing obj's class name to "Function", but equivalent unless someone has
 * overwritten the "Function" identifier with a different constructor and then
 * created instances using that constructor that might be passed in as obj).
 */
extern JS_PUBLIC_API(JSBool)
JS_ObjectIsFunction(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_ObjectIsCallable(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_IsNativeFunction(JSObject *funobj, JSNative call);

/* Return whether the given function is a valid constructor. */
extern JS_PUBLIC_API(JSBool)
JS_IsConstructor(JSFunction *fun);

/*
 * Bind the given callable to use the given object as "this".
 *
 * If |callable| is not callable, will throw and return NULL.
 */
extern JS_PUBLIC_API(JSObject*)
JS_BindCallable(JSContext *cx, JSObject *callable, JSObject *newThis);

extern JS_PUBLIC_API(JSBool)
JS_DefineFunctions(JSContext *cx, JSObject *obj, const JSFunctionSpec *fs);

extern JS_PUBLIC_API(JSFunction *)
JS_DefineFunction(JSContext *cx, JSObject *obj, const char *name, JSNative call,
                  unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API(JSFunction *)
JS_DefineUCFunction(JSContext *cx, JSObject *obj,
                    const jschar *name, size_t namelen, JSNative call,
                    unsigned nargs, unsigned attrs);

extern JS_PUBLIC_API(JSFunction *)
JS_DefineFunctionById(JSContext *cx, JSObject *obj, jsid id, JSNative call,
                      unsigned nargs, unsigned attrs);

/*
 * Clone a top-level function into a new scope. This function will dynamically
 * fail if funobj was lexically nested inside some other function.
 */
extern JS_PUBLIC_API(JSObject *)
JS_CloneFunctionObject(JSContext *cx, JSObject *funobj, JSObject *parent);

/*
 * Given a buffer, return JS_FALSE if the buffer might become a valid
 * javascript statement with the addition of more lines.  Otherwise return
 * JS_TRUE.  The intent is to support interactive compilation - accumulate
 * lines in a buffer until JS_BufferIsCompilableUnit is true, then pass it to
 * the compiler.
 */
extern JS_PUBLIC_API(JSBool)
JS_BufferIsCompilableUnit(JSContext *cx, JSObject *obj, const char *utf8, size_t length);

extern JS_PUBLIC_API(JSScript *)
JS_CompileScript(JSContext *cx, JSObject *obj,
                 const char *ascii, size_t length,
                 const char *filename, unsigned lineno);

extern JS_PUBLIC_API(JSScript *)
JS_CompileScriptForPrincipals(JSContext *cx, JSObject *obj,
                              JSPrincipals *principals,
                              const char *ascii, size_t length,
                              const char *filename, unsigned lineno);

extern JS_PUBLIC_API(JSScript *)
JS_CompileUCScript(JSContext *cx, JSObject *obj,
                   const jschar *chars, size_t length,
                   const char *filename, unsigned lineno);

extern JS_PUBLIC_API(JSScript *)
JS_CompileUCScriptForPrincipals(JSContext *cx, JSObject *obj,
                                JSPrincipals *principals,
                                const jschar *chars, size_t length,
                                const char *filename, unsigned lineno);

extern JS_PUBLIC_API(JSObject *)
JS_GetGlobalFromScript(JSScript *script);

extern JS_PUBLIC_API(JSFunction *)
JS_CompileFunction(JSContext *cx, JSObject *obj, const char *name,
                   unsigned nargs, const char **argnames,
                   const char *bytes, size_t length,
                   const char *filename, unsigned lineno);

extern JS_PUBLIC_API(JSFunction *)
JS_CompileFunctionForPrincipals(JSContext *cx, JSObject *obj,
                                JSPrincipals *principals, const char *name,
                                unsigned nargs, const char **argnames,
                                const char *bytes, size_t length,
                                const char *filename, unsigned lineno);

extern JS_PUBLIC_API(JSFunction *)
JS_CompileUCFunction(JSContext *cx, JSObject *obj, const char *name,
                     unsigned nargs, const char **argnames,
                     const jschar *chars, size_t length,
                     const char *filename, unsigned lineno);

namespace JS {

/* Options for JavaScript compilation. */
struct JS_PUBLIC_API(CompileOptions) {
    JSPrincipals *principals;
    JSPrincipals *originPrincipals;
    JSVersion version;
    bool versionSet;
    bool utf8;
    const char *filename;
    unsigned lineno;
    unsigned column;
    HandleObject element;
    bool compileAndGo;
    bool forEval;
    bool noScriptRval;
    bool selfHostingMode;
    bool canLazilyParse;
    enum SourcePolicy {
        NO_SOURCE,
        LAZY_SOURCE,
        SAVE_SOURCE
    } sourcePolicy;

    explicit CompileOptions(JSContext *cx, JSVersion version = JSVERSION_UNKNOWN);
    CompileOptions &setPrincipals(JSPrincipals *p) { principals = p; return *this; }
    CompileOptions &setOriginPrincipals(JSPrincipals *p) { originPrincipals = p; return *this; }
    CompileOptions &setVersion(JSVersion v) { version = v; versionSet = true; return *this; }
    CompileOptions &setUTF8(bool u) { utf8 = u; return *this; }
    CompileOptions &setFileAndLine(const char *f, unsigned l) {
        filename = f; lineno = l; return *this;
    }
    CompileOptions &setColumn(unsigned c) { column = c; return *this; }
    CompileOptions &setElement(HandleObject e) { element = e; return *this; }
    CompileOptions &setCompileAndGo(bool cng) { compileAndGo = cng; return *this; }
    CompileOptions &setForEval(bool eval) { forEval = eval; return *this; }
    CompileOptions &setNoScriptRval(bool nsr) { noScriptRval = nsr; return *this; }
    CompileOptions &setSelfHostingMode(bool shm) { selfHostingMode = shm; return *this; }
    CompileOptions &setCanLazilyParse(bool clp) { canLazilyParse = clp; return *this; }
    CompileOptions &setSourcePolicy(SourcePolicy sp) { sourcePolicy = sp; return *this; }
};

extern JS_PUBLIC_API(JSScript *)
Compile(JSContext *cx, JS::Handle<JSObject*> obj, CompileOptions options,
        const char *bytes, size_t length);

extern JS_PUBLIC_API(JSScript *)
Compile(JSContext *cx, JS::Handle<JSObject*> obj, CompileOptions options,
        const jschar *chars, size_t length);

extern JS_PUBLIC_API(JSScript *)
Compile(JSContext *cx, JS::Handle<JSObject*> obj, CompileOptions options, FILE *file);

extern JS_PUBLIC_API(JSScript *)
Compile(JSContext *cx, JS::Handle<JSObject*> obj, CompileOptions options, const char *filename);

extern JS_PUBLIC_API(JSFunction *)
CompileFunction(JSContext *cx, JS::Handle<JSObject*> obj, CompileOptions options,
                const char *name, unsigned nargs, const char **argnames,
                const char *bytes, size_t length);

extern JS_PUBLIC_API(JSFunction *)
CompileFunction(JSContext *cx, JS::Handle<JSObject*> obj, CompileOptions options,
                const char *name, unsigned nargs, const char **argnames,
                const jschar *chars, size_t length);

} /* namespace JS */

extern JS_PUBLIC_API(JSString *)
JS_DecompileScript(JSContext *cx, JSScript *script, const char *name, unsigned indent);

/*
 * API extension: OR this into indent to avoid pretty-printing the decompiled
 * source resulting from JS_DecompileFunction{,Body}.
 */
#define JS_DONT_PRETTY_PRINT    ((unsigned)0x8000)

extern JS_PUBLIC_API(JSString *)
JS_DecompileFunction(JSContext *cx, JSFunction *fun, unsigned indent);

extern JS_PUBLIC_API(JSString *)
JS_DecompileFunctionBody(JSContext *cx, JSFunction *fun, unsigned indent);

/*
 * NB: JS_ExecuteScript and the JS_Evaluate*Script* quadruplets use the obj
 * parameter as the initial scope chain header, the 'this' keyword value, and
 * the variables object (ECMA parlance for where 'var' and 'function' bind
 * names) of the execution context for script.
 *
 * Using obj as the variables object is problematic if obj's parent (which is
 * the scope chain link; see JS_SetParent and JS_NewObject) is not null: in
 * this case, variables created by 'var x = 0', e.g., go in obj, but variables
 * created by assignment to an unbound id, 'x = 0', go in the last object on
 * the scope chain linked by parent.
 *
 * ECMA calls that last scoping object the "global object", but note that many
 * embeddings have several such objects.  ECMA requires that "global code" be
 * executed with the variables object equal to this global object.  But these
 * JS API entry points provide freedom to execute code against a "sub-global",
 * i.e., a parented or scoped object, in which case the variables object will
 * differ from the last object on the scope chain, resulting in confusing and
 * non-ECMA explicit vs. implicit variable creation.
 *
 * Caveat embedders: unless you already depend on this buggy variables object
 * binding behavior, you should call JS_SetOptions(cx, JSOPTION_VAROBJFIX) or
 * JS_SetOptions(cx, JS_GetOptions(cx) | JSOPTION_VAROBJFIX) -- the latter if
 * someone may have set other options on cx already -- for each context in the
 * application, if you pass parented objects as the obj parameter, or may ever
 * pass such objects in the future.
 *
 * Why a runtime option?  The alternative is to add six or so new API entry
 * points with signatures matching the following six, and that doesn't seem
 * worth the code bloat cost.  Such new entry points would probably have less
 * obvious names, too, so would not tend to be used.  The JS_SetOption call,
 * OTOH, can be more easily hacked into existing code that does not depend on
 * the bug; such code can continue to use the familiar JS_EvaluateScript,
 * etc., entry points.
 */
extern JS_PUBLIC_API(JSBool)
JS_ExecuteScript(JSContext *cx, JSObject *obj, JSScript *script, jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_ExecuteScriptVersion(JSContext *cx, JSObject *obj, JSScript *script, jsval *rval,
                        JSVersion version);

/*
 * Execute either the function-defining prolog of a script, or the script's
 * main body, but not both.
 */
typedef enum JSExecPart { JSEXEC_PROLOG, JSEXEC_MAIN } JSExecPart;

extern JS_PUBLIC_API(JSBool)
JS_EvaluateScript(JSContext *cx, JSObject *obj,
                  const char *bytes, unsigned length,
                  const char *filename, unsigned lineno,
                  jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_EvaluateScriptForPrincipals(JSContext *cx, JSObject *obj,
                               JSPrincipals *principals,
                               const char *bytes, unsigned length,
                               const char *filename, unsigned lineno,
                               jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_EvaluateScriptForPrincipalsVersion(JSContext *cx, JSObject *obj,
                                      JSPrincipals *principals,
                                      const char *bytes, unsigned length,
                                      const char *filename, unsigned lineno,
                                      jsval *rval, JSVersion version);

extern JS_PUBLIC_API(JSBool)
JS_EvaluateUCScript(JSContext *cx, JSObject *obj,
                    const jschar *chars, unsigned length,
                    const char *filename, unsigned lineno,
                    jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_EvaluateUCScriptForPrincipals(JSContext *cx, JSObject *obj,
                                 JSPrincipals *principals,
                                 const jschar *chars, unsigned length,
                                 const char *filename, unsigned lineno,
                                 jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_EvaluateUCScriptForPrincipalsVersion(JSContext *cx, JSObject *obj,
                                        JSPrincipals *principals,
                                        const jschar *chars, unsigned length,
                                        const char *filename, unsigned lineno,
                                        jsval *rval, JSVersion version);

/*
 * JSAPI clients may optionally specify the 'originPrincipals' of a script.
 * A script's originPrincipals may be retrieved through the debug API (via
 * JS_GetScriptOriginPrincipals) and the originPrincipals are transitively
 * assigned to any nested scripts (including scripts dynamically created via
 * eval and the Function constructor). If originPrincipals is null, then the
 * value of principals is used as origin principals for the script.
 */
extern JS_PUBLIC_API(JSBool)
JS_EvaluateUCScriptForPrincipalsVersionOrigin(JSContext *cx, JSObject *obj,
                                              JSPrincipals *principals,
                                              JSPrincipals *originPrincipals,
                                              const jschar *chars, unsigned length,
                                              const char *filename, unsigned lineno,
                                              jsval *rval, JSVersion version);

namespace JS {

extern JS_PUBLIC_API(bool)
Evaluate(JSContext *cx, JS::Handle<JSObject*> obj, CompileOptions options,
         const jschar *chars, size_t length, jsval *rval);

extern JS_PUBLIC_API(bool)
Evaluate(JSContext *cx, JS::Handle<JSObject*> obj, CompileOptions options,
         const char *bytes, size_t length, jsval *rval);

extern JS_PUBLIC_API(bool)
Evaluate(JSContext *cx, JS::Handle<JSObject*> obj, CompileOptions options,
         const char *filename, jsval *rval);

} /* namespace JS */

extern JS_PUBLIC_API(JSBool)
JS_CallFunction(JSContext *cx, JSObject *obj, JSFunction *fun, unsigned argc,
                jsval *argv, jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_CallFunctionName(JSContext *cx, JSObject *obj, const char *name, unsigned argc,
                    jsval *argv, jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_CallFunctionValue(JSContext *cx, JSObject *obj, jsval fval, unsigned argc,
                     jsval *argv, jsval *rval);

namespace JS {

static inline bool
Call(JSContext *cx, JSObject *thisObj, JSFunction *fun, unsigned argc, jsval *argv, jsval *rval) {
    return !!JS_CallFunction(cx, thisObj, fun, argc, argv, rval);
}

static inline bool
Call(JSContext *cx, JSObject *thisObj, const char *name, unsigned argc, jsval *argv, jsval *rval) {
    return !!JS_CallFunctionName(cx, thisObj, name, argc, argv, rval);
}

static inline bool
Call(JSContext *cx, JSObject *thisObj, jsval fun, unsigned argc, jsval *argv, jsval *rval) {
    return !!JS_CallFunctionValue(cx, thisObj, fun, argc, argv, rval);
}

extern JS_PUBLIC_API(bool)
Call(JSContext *cx, jsval thisv, jsval fun, unsigned argc, jsval *argv, jsval *rval);

static inline bool
Call(JSContext *cx, jsval thisv, JSObject *funObj, unsigned argc, jsval *argv, jsval *rval) {
    return Call(cx, thisv, OBJECT_TO_JSVAL(funObj), argc, argv, rval);
}

} /* namespace JS */

/*
 * These functions allow setting an operation callback that will be called
 * from the JS thread some time after any thread triggered the callback using
 * JS_TriggerOperationCallback(rt).
 *
 * To schedule the GC and for other activities the engine internally triggers
 * operation callbacks. The embedding should thus not rely on callbacks being
 * triggered through the external API only.
 *
 * Important note: Additional callbacks can occur inside the callback handler
 * if it re-enters the JS engine. The embedding must ensure that the callback
 * is disconnected before attempting such re-entry.
 */
extern JS_PUBLIC_API(JSOperationCallback)
JS_SetOperationCallback(JSContext *cx, JSOperationCallback callback);

extern JS_PUBLIC_API(JSOperationCallback)
JS_GetOperationCallback(JSContext *cx);

extern JS_PUBLIC_API(void)
JS_TriggerOperationCallback(JSRuntime *rt);

extern JS_PUBLIC_API(JSBool)
JS_IsRunning(JSContext *cx);

/*
 * Saving and restoring frame chains.
 *
 * These two functions are used to set aside cx's call stack while that stack
 * is inactive. After a call to JS_SaveFrameChain, it looks as if there is no
 * code running on cx. Before calling JS_RestoreFrameChain, cx's call stack
 * must be balanced and all nested calls to JS_SaveFrameChain must have had
 * matching JS_RestoreFrameChain calls.
 *
 * JS_SaveFrameChain deals with cx not having any code running on it.
 */
extern JS_PUBLIC_API(JSBool)
JS_SaveFrameChain(JSContext *cx);

extern JS_PUBLIC_API(void)
JS_RestoreFrameChain(JSContext *cx);

#ifdef MOZ_TRACE_JSCALLS
/*
 * The callback is expected to be quick and noninvasive. It should not
 * trigger interrupts, turn on debugging, or produce uncaught JS
 * exceptions. The state of the stack and registers in the context
 * cannot be relied upon, since this callback may be invoked directly
 * from either JIT. The 'entering' field means we are entering a
 * function if it is positive, leaving a function if it is zero or
 * negative.
 */
extern JS_PUBLIC_API(void)
JS_SetFunctionCallback(JSContext *cx, JSFunctionCallback fcb);

extern JS_PUBLIC_API(JSFunctionCallback)
JS_GetFunctionCallback(JSContext *cx);
#endif /* MOZ_TRACE_JSCALLS */

/************************************************************************/

/*
 * Strings.
 *
 * NB: JS_NewUCString takes ownership of bytes on success, avoiding a copy;
 * but on error (signified by null return), it leaves chars owned by the
 * caller. So the caller must free bytes in the error case, if it has no use
 * for them. In contrast, all the JS_New*StringCopy* functions do not take
 * ownership of the character memory passed to them -- they copy it.
 */
extern JS_PUBLIC_API(JSString *)
JS_NewStringCopyN(JSContext *cx, const char *s, size_t n);

extern JS_PUBLIC_API(JSString *)
JS_NewStringCopyZ(JSContext *cx, const char *s);

extern JS_PUBLIC_API(JSString *)
JS_InternJSString(JSContext *cx, JSString *str);

extern JS_PUBLIC_API(JSString *)
JS_InternStringN(JSContext *cx, const char *s, size_t length);

extern JS_PUBLIC_API(JSString *)
JS_InternString(JSContext *cx, const char *s);

extern JS_PUBLIC_API(JSString *)
JS_NewUCString(JSContext *cx, jschar *chars, size_t length);

extern JS_PUBLIC_API(JSString *)
JS_NewUCStringCopyN(JSContext *cx, const jschar *s, size_t n);

extern JS_PUBLIC_API(JSString *)
JS_NewUCStringCopyZ(JSContext *cx, const jschar *s);

extern JS_PUBLIC_API(JSString *)
JS_InternUCStringN(JSContext *cx, const jschar *s, size_t length);

extern JS_PUBLIC_API(JSString *)
JS_InternUCString(JSContext *cx, const jschar *s);

extern JS_PUBLIC_API(JSBool)
JS_CompareStrings(JSContext *cx, JSString *str1, JSString *str2, int32_t *result);

extern JS_PUBLIC_API(JSBool)
JS_StringEqualsAscii(JSContext *cx, JSString *str, const char *asciiBytes, JSBool *match);

extern JS_PUBLIC_API(size_t)
JS_PutEscapedString(JSContext *cx, char *buffer, size_t size, JSString *str, char quote);

extern JS_PUBLIC_API(JSBool)
JS_FileEscapedString(FILE *fp, JSString *str, char quote);

/*
 * Extracting string characters and length.
 *
 * While getting the length of a string is infallible, getting the chars can
 * fail. As indicated by the lack of a JSContext parameter, there are two
 * special cases where getting the chars is infallible:
 *
 * The first case is interned strings, i.e., strings from JS_InternString or
 * JSID_TO_STRING(id), using JS_GetInternedStringChars*.
 *
 * The second case is "flat" strings that have been explicitly prepared in a
 * fallible context by JS_FlattenString. To catch errors, a separate opaque
 * JSFlatString type is returned by JS_FlattenString and expected by
 * JS_GetFlatStringChars. Note, though, that this is purely a syntactic
 * distinction: the input and output of JS_FlattenString are the same actual
 * GC-thing so only one needs to be rooted. If a JSString is known to be flat,
 * JS_ASSERT_STRING_IS_FLAT can be used to make a debug-checked cast. Example:
 *
 *   // in a fallible context
 *   JSFlatString *fstr = JS_FlattenString(cx, str);
 *   if (!fstr)
 *     return JS_FALSE;
 *   JS_ASSERT(fstr == JS_ASSERT_STRING_IS_FLAT(str));
 *
 *   // in an infallible context, for the same 'str'
 *   const jschar *chars = JS_GetFlatStringChars(fstr)
 *   JS_ASSERT(chars);
 *
 * The CharsZ APIs guarantee that the returned array has a null character at
 * chars[length]. This can require additional copying so clients should prefer
 * APIs without CharsZ if possible. The infallible functions also return
 * null-terminated arrays. (There is no additional cost or non-Z alternative
 * for the infallible functions, so 'Z' is left out of the identifier.)
 */

extern JS_PUBLIC_API(size_t)
JS_GetStringLength(JSString *str);

extern JS_PUBLIC_API(const jschar *)
JS_GetStringCharsAndLength(JSContext *cx, JSString *str, size_t *length);

extern JS_PUBLIC_API(const jschar *)
JS_GetInternedStringChars(JSString *str);

extern JS_PUBLIC_API(const jschar *)
JS_GetInternedStringCharsAndLength(JSString *str, size_t *length);

extern JS_PUBLIC_API(const jschar *)
JS_GetStringCharsZ(JSContext *cx, JSString *str);

extern JS_PUBLIC_API(const jschar *)
JS_GetStringCharsZAndLength(JSContext *cx, JSString *str, size_t *length);

extern JS_PUBLIC_API(JSFlatString *)
JS_FlattenString(JSContext *cx, JSString *str);

extern JS_PUBLIC_API(const jschar *)
JS_GetFlatStringChars(JSFlatString *str);

static JS_ALWAYS_INLINE JSFlatString *
JSID_TO_FLAT_STRING(jsid id)
{
    JS_ASSERT(JSID_IS_STRING(id));
    return (JSFlatString *)(JSID_BITS(id));
}

static JS_ALWAYS_INLINE JSFlatString *
JS_ASSERT_STRING_IS_FLAT(JSString *str)
{
    JS_ASSERT(JS_GetFlatStringChars((JSFlatString *)str));
    return (JSFlatString *)str;
}

static JS_ALWAYS_INLINE JSString *
JS_FORGET_STRING_FLATNESS(JSFlatString *fstr)
{
    return (JSString *)fstr;
}

/*
 * Additional APIs that avoid fallibility when given a flat string.
 */

extern JS_PUBLIC_API(JSBool)
JS_FlatStringEqualsAscii(JSFlatString *str, const char *asciiBytes);

extern JS_PUBLIC_API(size_t)
JS_PutEscapedFlatString(char *buffer, size_t size, JSFlatString *str, char quote);

/*
 * This function is now obsolete and behaves the same as JS_NewUCString.  Use
 * JS_NewUCString instead.
 */
extern JS_PUBLIC_API(JSString *)
JS_NewGrowableString(JSContext *cx, jschar *chars, size_t length);

/*
 * Create a dependent string, i.e., a string that owns no character storage,
 * but that refers to a slice of another string's chars.  Dependent strings
 * are mutable by definition, so the thread safety comments above apply.
 */
extern JS_PUBLIC_API(JSString *)
JS_NewDependentString(JSContext *cx, JSString *str, size_t start,
                      size_t length);

/*
 * Concatenate two strings, possibly resulting in a rope.
 * See above for thread safety comments.
 */
extern JS_PUBLIC_API(JSString *)
JS_ConcatStrings(JSContext *cx, JSString *left, JSString *right);

/*
 * For JS_DecodeBytes, set *dstlenp to the size of the destination buffer before
 * the call; on return, *dstlenp contains the number of jschars actually stored.
 * To determine the necessary destination buffer size, make a sizing call that
 * passes NULL for dst.
 *
 * On errors, the functions report the error. In that case, *dstlenp contains
 * the number of characters or bytes transferred so far.  If cx is NULL, no
 * error is reported on failure, and the functions simply return JS_FALSE.
 *
 * NB: This function does not store an additional zero byte or jschar after the
 * transcoded string.
 */
JS_PUBLIC_API(JSBool)
JS_DecodeBytes(JSContext *cx, const char *src, size_t srclen, jschar *dst,
               size_t *dstlenp);

/*
 * A variation on JS_EncodeCharacters where a null terminated string is
 * returned that you are expected to call JS_free on when done.
 */
JS_PUBLIC_API(char *)
JS_EncodeString(JSContext *cx, JSString *str);

/*
 * Same behavior as JS_EncodeString(), but encode into UTF-8 string
 */
JS_PUBLIC_API(char *)
JS_EncodeStringToUTF8(JSContext *cx, JSString *str);

/*
 * Get number of bytes in the string encoding (without accounting for a
 * terminating zero bytes. The function returns (size_t) -1 if the string
 * can not be encoded into bytes and reports an error using cx accordingly.
 */
JS_PUBLIC_API(size_t)
JS_GetStringEncodingLength(JSContext *cx, JSString *str);

/*
 * Encode string into a buffer. The function does not stores an additional
 * zero byte. The function returns (size_t) -1 if the string can not be
 * encoded into bytes with no error reported. Otherwise it returns the number
 * of bytes that are necessary to encode the string. If that exceeds the
 * length parameter, the string will be cut and only length bytes will be
 * written into the buffer.
 */
JS_PUBLIC_API(size_t)
JS_EncodeStringToBuffer(JSContext *cx, JSString *str, char *buffer, size_t length);

class JSAutoByteString
{
  public:
    JSAutoByteString(JSContext *cx, JSString *str
                     MOZ_GUARD_OBJECT_NOTIFIER_PARAM)
      : mBytes(JS_EncodeString(cx, str))
    {
        JS_ASSERT(cx);
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    JSAutoByteString(MOZ_GUARD_OBJECT_NOTIFIER_ONLY_PARAM)
      : mBytes(NULL)
    {
        MOZ_GUARD_OBJECT_NOTIFIER_INIT;
    }

    ~JSAutoByteString() {
        js_free(mBytes);
    }

    /* Take ownership of the given byte array. */
    void initBytes(char *bytes) {
        JS_ASSERT(!mBytes);
        mBytes = bytes;
    }

    char *encodeLatin1(JSContext *cx, JSString *str) {
        JS_ASSERT(!mBytes);
        JS_ASSERT(cx);
        mBytes = JS_EncodeString(cx, str);
        return mBytes;
    }

    char *encodeUtf8(JSContext *cx, JSString *str) {
        JS_ASSERT(!mBytes);
        JS_ASSERT(cx);
        mBytes = JS_EncodeStringToUTF8(cx, str);
        return mBytes;
    }

    void clear() {
        js_free(mBytes);
        mBytes = NULL;
    }

    char *ptr() const {
        return mBytes;
    }

    bool operator!() const {
        return !mBytes;
    }

    size_t length() const {
        if (!mBytes)
            return 0;
        return strlen(mBytes);
    }

  private:
    char        *mBytes;
    MOZ_DECL_USE_GUARD_OBJECT_NOTIFIER

    /* Copy and assignment are not supported. */
    JSAutoByteString(const JSAutoByteString &another);
    JSAutoByteString &operator=(const JSAutoByteString &another);
};

/************************************************************************/
/*
 * JSON functions
 */
typedef JSBool (* JSONWriteCallback)(const jschar *buf, uint32_t len, void *data);

/*
 * JSON.stringify as specified by ES5.
 */
JS_PUBLIC_API(JSBool)
JS_Stringify(JSContext *cx, jsval *vp, JSObject *replacer, jsval space,
             JSONWriteCallback callback, void *data);

/*
 * JSON.parse as specified by ES5.
 */
JS_PUBLIC_API(JSBool)
JS_ParseJSON(JSContext *cx, const jschar *chars, uint32_t len, JS::MutableHandle<JS::Value> vp);

JS_PUBLIC_API(JSBool)
JS_ParseJSONWithReviver(JSContext *cx, const jschar *chars, uint32_t len, jsval reviver,
                        jsval *vp);

/************************************************************************/

/* API for the HTML5 internal structured cloning algorithm. */

/* The maximum supported structured-clone serialization format version. */
#define JS_STRUCTURED_CLONE_VERSION 2

struct JSStructuredCloneCallbacks {
    ReadStructuredCloneOp read;
    WriteStructuredCloneOp write;
    StructuredCloneErrorOp reportError;
};

/* Note: if the *data contains transferable objects, it can be read
 * only once */
JS_PUBLIC_API(JSBool)
JS_ReadStructuredClone(JSContext *cx, uint64_t *data, size_t nbytes,
                       uint32_t version, jsval *vp,
                       const JSStructuredCloneCallbacks *optionalCallbacks,
                       void *closure);

/* Note: On success, the caller is responsible for calling
 * JS_ClearStructuredClone(*datap, nbytesp). */
JS_PUBLIC_API(JSBool)
JS_WriteStructuredClone(JSContext *cx, jsval v, uint64_t **datap, size_t *nbytesp,
                        const JSStructuredCloneCallbacks *optionalCallbacks,
                        void *closure, jsval transferable);

JS_PUBLIC_API(JSBool)
JS_ClearStructuredClone(const uint64_t *data, size_t nbytes);

JS_PUBLIC_API(JSBool)
JS_StructuredCloneHasTransferables(const uint64_t *data, size_t nbytes,
                                   JSBool *hasTransferable);

JS_PUBLIC_API(JSBool)
JS_StructuredClone(JSContext *cx, jsval v, jsval *vp,
                   const JSStructuredCloneCallbacks *optionalCallbacks,
                   void *closure);

/* RAII sugar for JS_WriteStructuredClone. */
class JS_PUBLIC_API(JSAutoStructuredCloneBuffer) {
    uint64_t *data_;
    size_t nbytes_;
    uint32_t version_;

  public:
    JSAutoStructuredCloneBuffer()
        : data_(NULL), nbytes_(0), version_(JS_STRUCTURED_CLONE_VERSION) {}

    ~JSAutoStructuredCloneBuffer() { clear(); }

    uint64_t *data() const { return data_; }
    size_t nbytes() const { return nbytes_; }

    void clear();

    /* Copy some memory. It will be automatically freed by the destructor. */
    bool copy(const uint64_t *data, size_t nbytes, uint32_t version=JS_STRUCTURED_CLONE_VERSION);

    /*
     * Adopt some memory. It will be automatically freed by the destructor.
     * data must have been allocated by the JS engine (e.g., extracted via
     * JSAutoStructuredCloneBuffer::steal).
     */
    void adopt(uint64_t *data, size_t nbytes, uint32_t version=JS_STRUCTURED_CLONE_VERSION);

    /*
     * Remove the buffer so that it will not be automatically freed.
     * After this, the caller is responsible for feeding the memory back to
     * JSAutoStructuredCloneBuffer::adopt.
     */
    void steal(uint64_t **datap, size_t *nbytesp, uint32_t *versionp=NULL);

    bool read(JSContext *cx, jsval *vp,
              const JSStructuredCloneCallbacks *optionalCallbacks=NULL,
              void *closure=NULL);

    bool write(JSContext *cx, jsval v,
               const JSStructuredCloneCallbacks *optionalCallbacks=NULL,
               void *closure=NULL);

    bool write(JSContext *cx, jsval v,
               jsval transferable,
               const JSStructuredCloneCallbacks *optionalCallbacks=NULL,
               void *closure=NULL);

    /**
     * Swap ownership with another JSAutoStructuredCloneBuffer.
     */
    void swap(JSAutoStructuredCloneBuffer &other);

  private:
    /* Copy and assignment are not supported. */
    JSAutoStructuredCloneBuffer(const JSAutoStructuredCloneBuffer &other);
    JSAutoStructuredCloneBuffer &operator=(const JSAutoStructuredCloneBuffer &other);
};

/* API for implementing custom serialization behavior (for ImageData, File, etc.) */

/* The range of tag values the application may use for its own custom object types. */
#define JS_SCTAG_USER_MIN  ((uint32_t) 0xFFFF8000)
#define JS_SCTAG_USER_MAX  ((uint32_t) 0xFFFFFFFF)

#define JS_SCERR_RECURSION 0
#define JS_SCERR_TRANSFERABLE 1

JS_PUBLIC_API(void)
JS_SetStructuredCloneCallbacks(JSRuntime *rt, const JSStructuredCloneCallbacks *callbacks);

JS_PUBLIC_API(JSBool)
JS_ReadUint32Pair(JSStructuredCloneReader *r, uint32_t *p1, uint32_t *p2);

JS_PUBLIC_API(JSBool)
JS_ReadBytes(JSStructuredCloneReader *r, void *p, size_t len);

JS_PUBLIC_API(JSBool)
JS_ReadTypedArray(JSStructuredCloneReader *r, jsval *vp);

JS_PUBLIC_API(JSBool)
JS_WriteUint32Pair(JSStructuredCloneWriter *w, uint32_t tag, uint32_t data);

JS_PUBLIC_API(JSBool)
JS_WriteBytes(JSStructuredCloneWriter *w, const void *p, size_t len);

JS_PUBLIC_API(JSBool)
JS_WriteTypedArray(JSStructuredCloneWriter *w, jsval v);

/************************************************************************/

/*
 * The default locale for the ECMAScript Internationalization API
 * (Intl.Collator, Intl.NumberFormat, Intl.DateTimeFormat).
 * Note that the Internationalization API encourages clients to
 * specify their own locales.
 * The locale string remains owned by the caller.
 */
extern JS_PUBLIC_API(JSBool)
JS_SetDefaultLocale(JSRuntime *rt, const char *locale);

/*
 * Reset the default locale to OS defaults.
 */
extern JS_PUBLIC_API(void)
JS_ResetDefaultLocale(JSRuntime *rt);

/*
 * Locale specific string conversion and error message callbacks.
 */
struct JSLocaleCallbacks {
    JSLocaleToUpperCase     localeToUpperCase;
    JSLocaleToLowerCase     localeToLowerCase;
    JSLocaleCompare         localeCompare; // not used #if ENABLE_INTL_API
    JSLocaleToUnicode       localeToUnicode;
    JSErrorCallback         localeGetErrorMessage;
};

/*
 * Establish locale callbacks. The pointer must persist as long as the
 * JSRuntime.  Passing NULL restores the default behaviour.
 */
extern JS_PUBLIC_API(void)
JS_SetLocaleCallbacks(JSRuntime *rt, JSLocaleCallbacks *callbacks);

/*
 * Return the address of the current locale callbacks struct, which may
 * be NULL.
 */
extern JS_PUBLIC_API(JSLocaleCallbacks *)
JS_GetLocaleCallbacks(JSRuntime *rt);

/************************************************************************/

/*
 * Error reporting.
 */

/*
 * Report an exception represented by the sprintf-like conversion of format
 * and its arguments.  This exception message string is passed to a pre-set
 * JSErrorReporter function (set by JS_SetErrorReporter).
 */
extern JS_PUBLIC_API(void)
JS_ReportError(JSContext *cx, const char *format, ...);

/*
 * Use an errorNumber to retrieve the format string, args are char *
 */
extern JS_PUBLIC_API(void)
JS_ReportErrorNumber(JSContext *cx, JSErrorCallback errorCallback,
                     void *userRef, const unsigned errorNumber, ...);

#ifdef va_start
extern JS_PUBLIC_API(void)
JS_ReportErrorNumberVA(JSContext *cx, JSErrorCallback errorCallback,
                       void *userRef, const unsigned errorNumber, va_list ap);
#endif

/*
 * Use an errorNumber to retrieve the format string, args are jschar *
 */
extern JS_PUBLIC_API(void)
JS_ReportErrorNumberUC(JSContext *cx, JSErrorCallback errorCallback,
                     void *userRef, const unsigned errorNumber, ...);

extern JS_PUBLIC_API(void)
JS_ReportErrorNumberUCArray(JSContext *cx, JSErrorCallback errorCallback,
                            void *userRef, const unsigned errorNumber,
                            const jschar **args);

/*
 * As above, but report a warning instead (JSREPORT_IS_WARNING(report.flags)).
 * Return true if there was no error trying to issue the warning, and if the
 * warning was not converted into an error due to the JSOPTION_WERROR option
 * being set, false otherwise.
 */
extern JS_PUBLIC_API(JSBool)
JS_ReportWarning(JSContext *cx, const char *format, ...);

extern JS_PUBLIC_API(JSBool)
JS_ReportErrorFlagsAndNumber(JSContext *cx, unsigned flags,
                             JSErrorCallback errorCallback, void *userRef,
                             const unsigned errorNumber, ...);

extern JS_PUBLIC_API(JSBool)
JS_ReportErrorFlagsAndNumberUC(JSContext *cx, unsigned flags,
                               JSErrorCallback errorCallback, void *userRef,
                               const unsigned errorNumber, ...);

/*
 * Complain when out of memory.
 */
extern JS_PUBLIC_API(void)
JS_ReportOutOfMemory(JSContext *cx);

/*
 * Complain when an allocation size overflows the maximum supported limit.
 */
extern JS_PUBLIC_API(void)
JS_ReportAllocationOverflow(JSContext *cx);

struct JSErrorReport {
    const char      *filename;      /* source file name, URL, etc., or null */
    JSPrincipals    *originPrincipals; /* see 'originPrincipals' comment above */
    unsigned        lineno;         /* source line number */
    const char      *linebuf;       /* offending source line without final \n */
    const char      *tokenptr;      /* pointer to error token in linebuf */
    const jschar    *uclinebuf;     /* unicode (original) line buffer */
    const jschar    *uctokenptr;    /* unicode (original) token pointer */
    unsigned        flags;          /* error/warning, etc. */
    unsigned        errorNumber;    /* the error number, e.g. see js.msg */
    const jschar    *ucmessage;     /* the (default) error message */
    const jschar    **messageArgs;  /* arguments for the error message */
    int16_t         exnType;        /* One of the JSExnType constants */
    unsigned        column;         /* zero-based column index in line */
};

/*
 * JSErrorReport flag values.  These may be freely composed.
 */
#define JSREPORT_ERROR      0x0     /* pseudo-flag for default case */
#define JSREPORT_WARNING    0x1     /* reported via JS_ReportWarning */
#define JSREPORT_EXCEPTION  0x2     /* exception was thrown */
#define JSREPORT_STRICT     0x4     /* error or warning due to strict option */

/*
 * This condition is an error in strict mode code, a warning if
 * JS_HAS_STRICT_OPTION(cx), and otherwise should not be reported at
 * all.  We check the strictness of the context's top frame's script;
 * where that isn't appropriate, the caller should do the right checks
 * itself instead of using this flag.
 */
#define JSREPORT_STRICT_MODE_ERROR 0x8

/*
 * If JSREPORT_EXCEPTION is set, then a JavaScript-catchable exception
 * has been thrown for this runtime error, and the host should ignore it.
 * Exception-aware hosts should also check for JS_IsExceptionPending if
 * JS_ExecuteScript returns failure, and signal or propagate the exception, as
 * appropriate.
 */
#define JSREPORT_IS_WARNING(flags)      (((flags) & JSREPORT_WARNING) != 0)
#define JSREPORT_IS_EXCEPTION(flags)    (((flags) & JSREPORT_EXCEPTION) != 0)
#define JSREPORT_IS_STRICT(flags)       (((flags) & JSREPORT_STRICT) != 0)
#define JSREPORT_IS_STRICT_MODE_ERROR(flags) (((flags) &                      \
                                              JSREPORT_STRICT_MODE_ERROR) != 0)
extern JS_PUBLIC_API(JSErrorReporter)
JS_GetErrorReporter(JSContext *cx);

extern JS_PUBLIC_API(JSErrorReporter)
JS_SetErrorReporter(JSContext *cx, JSErrorReporter er);

/************************************************************************/

/*
 * Dates.
 */

extern JS_PUBLIC_API(JSObject *)
JS_NewDateObject(JSContext *cx, int year, int mon, int mday, int hour, int min, int sec);

extern JS_PUBLIC_API(JSObject *)
JS_NewDateObjectMsec(JSContext *cx, double msec);

/*
 * Infallible predicate to test whether obj is a date object.
 */
extern JS_PUBLIC_API(JSBool)
JS_ObjectIsDate(JSContext *cx, JSObject *obj);

/*
 * Clears the cache of calculated local time from each Date object.
 * Call to propagate a system timezone change.
 */
extern JS_PUBLIC_API(void)
JS_ClearDateCaches(JSContext *cx);

/************************************************************************/

/*
 * Regular Expressions.
 */
#define JSREG_FOLD      0x01    /* fold uppercase to lowercase */
#define JSREG_GLOB      0x02    /* global exec, creates array of matches */
#define JSREG_MULTILINE 0x04    /* treat ^ and $ as begin and end of line */
#define JSREG_STICKY    0x08    /* only match starting at lastIndex */

extern JS_PUBLIC_API(JSObject *)
JS_NewRegExpObject(JSContext *cx, JSObject *obj, char *bytes, size_t length, unsigned flags);

extern JS_PUBLIC_API(JSObject *)
JS_NewUCRegExpObject(JSContext *cx, JSObject *obj, jschar *chars, size_t length, unsigned flags);

extern JS_PUBLIC_API(void)
JS_SetRegExpInput(JSContext *cx, JSObject *obj, JSString *input, JSBool multiline);

extern JS_PUBLIC_API(void)
JS_ClearRegExpStatics(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(JSBool)
JS_ExecuteRegExp(JSContext *cx, JSObject *obj, JSObject *reobj, jschar *chars, size_t length,
                 size_t *indexp, JSBool test, jsval *rval);

/* RegExp interface for clients without a global object. */

extern JS_PUBLIC_API(JSObject *)
JS_NewRegExpObjectNoStatics(JSContext *cx, char *bytes, size_t length, unsigned flags);

extern JS_PUBLIC_API(JSObject *)
JS_NewUCRegExpObjectNoStatics(JSContext *cx, jschar *chars, size_t length, unsigned flags);

extern JS_PUBLIC_API(JSBool)
JS_ExecuteRegExpNoStatics(JSContext *cx, JSObject *reobj, jschar *chars, size_t length,
                          size_t *indexp, JSBool test, jsval *rval);

extern JS_PUBLIC_API(JSBool)
JS_ObjectIsRegExp(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(unsigned)
JS_GetRegExpFlags(JSContext *cx, JSObject *obj);

extern JS_PUBLIC_API(JSString *)
JS_GetRegExpSource(JSContext *cx, JSObject *obj);

/************************************************************************/

extern JS_PUBLIC_API(JSBool)
JS_IsExceptionPending(JSContext *cx);

extern JS_PUBLIC_API(JSBool)
JS_GetPendingException(JSContext *cx, jsval *vp);

extern JS_PUBLIC_API(void)
JS_SetPendingException(JSContext *cx, jsval v);

extern JS_PUBLIC_API(void)
JS_ClearPendingException(JSContext *cx);

extern JS_PUBLIC_API(JSBool)
JS_ReportPendingException(JSContext *cx);

/*
 * Save the current exception state.  This takes a snapshot of cx's current
 * exception state without making any change to that state.
 *
 * The returned state pointer MUST be passed later to JS_RestoreExceptionState
 * (to restore that saved state, overriding any more recent state) or else to
 * JS_DropExceptionState (to free the state struct in case it is not correct
 * or desirable to restore it).  Both Restore and Drop free the state struct,
 * so callers must stop using the pointer returned from Save after calling the
 * Release or Drop API.
 */
extern JS_PUBLIC_API(JSExceptionState *)
JS_SaveExceptionState(JSContext *cx);

extern JS_PUBLIC_API(void)
JS_RestoreExceptionState(JSContext *cx, JSExceptionState *state);

extern JS_PUBLIC_API(void)
JS_DropExceptionState(JSContext *cx, JSExceptionState *state);

/*
 * If the given value is an exception object that originated from an error,
 * the exception will contain an error report struct, and this API will return
 * the address of that struct.  Otherwise, it returns NULL.  The lifetime of
 * the error report struct that might be returned is the same as the lifetime
 * of the exception object.
 */
extern JS_PUBLIC_API(JSErrorReport *)
JS_ErrorFromException(JSContext *cx, jsval v);

/*
 * Given a reported error's message and JSErrorReport struct pointer, throw
 * the corresponding exception on cx.
 */
extern JS_PUBLIC_API(JSBool)
JS_ThrowReportedError(JSContext *cx, const char *message,
                      JSErrorReport *reportp);

/*
 * Throws a StopIteration exception on cx.
 */
extern JS_PUBLIC_API(JSBool)
JS_ThrowStopIteration(JSContext *cx);

extern JS_PUBLIC_API(intptr_t)
JS_GetCurrentThread();

/*
 * A JS runtime always has an "owner thread". The owner thread is set when the
 * runtime is created (to the current thread) and practically all entry points
 * into the JS engine check that a runtime (or anything contained in the
 * runtime: context, compartment, object, etc) is only touched by its owner
 * thread. Embeddings may check this invariant outside the JS engine by calling
 * JS_AbortIfWrongThread (which will abort if not on the owner thread, even for
 * non-debug builds).
 *
 * It is possible to "move" a runtime between threads. This is accomplished by
 * calling JS_ClearRuntimeThread on a runtime's owner thread and then calling
 * JS_SetRuntimeThread on the new owner thread. The runtime must not be
 * accessed between JS_ClearRuntimeThread and JS_SetRuntimeThread. Also, the
 * caller is responsible for synchronizing the calls to Set/Clear.
 */

extern JS_PUBLIC_API(void)
JS_AbortIfWrongThread(JSRuntime *rt);

extern JS_PUBLIC_API(void)
JS_ClearRuntimeThread(JSRuntime *rt);

extern JS_PUBLIC_API(void)
JS_SetRuntimeThread(JSRuntime *rt);

class JSAutoSetRuntimeThread
{
    JSRuntime *runtime_;

  public:
    JSAutoSetRuntimeThread(JSRuntime *runtime) : runtime_(runtime) {
        JS_SetRuntimeThread(runtime_);
    }

    ~JSAutoSetRuntimeThread() {
        JS_ClearRuntimeThread(runtime_);
    }
};

/************************************************************************/

/*
 * JS_IsConstructing must be called from within a native given the
 * native's original cx and vp arguments. If JS_IsConstructing is true,
 * JS_THIS must not be used; the constructor should construct and return a
 * new object. Otherwise, the native is called as an ordinary function and
 * JS_THIS may be used.
 */
static JS_ALWAYS_INLINE JSBool
JS_IsConstructing(JSContext *cx, const jsval *vp)
{
#ifdef DEBUG
    JSObject *callee = JSVAL_TO_OBJECT(JS_CALLEE(cx, vp));
    if (JS_ObjectIsFunction(cx, callee)) {
        JSFunction *fun = JS_ValueToFunction(cx, JS_CALLEE(cx, vp));
        JS_ASSERT(JS_IsConstructor(fun));
    } else {
        JS_ASSERT(JS_GetClass(callee)->construct != NULL);
    }
#else
    (void)cx;
#endif

    return JSVAL_IS_MAGIC_IMPL(JSVAL_TO_IMPL(vp[1]));
}

/*
 * A constructor can request that the JS engine create a default new 'this'
 * object of the given class, using the callee to determine parentage and
 * [[Prototype]].
 */
extern JS_PUBLIC_API(JSObject *)
JS_NewObjectForConstructor(JSContext *cx, JSClass *clasp, const jsval *vp);

/************************************************************************/

#ifdef JS_GC_ZEAL
#define JS_DEFAULT_ZEAL_FREQ 100

extern JS_PUBLIC_API(void)
JS_SetGCZeal(JSContext *cx, uint8_t zeal, uint32_t frequency);

extern JS_PUBLIC_API(void)
JS_ScheduleGC(JSContext *cx, uint32_t count);
#endif

extern JS_PUBLIC_API(void)
JS_SetParallelCompilationEnabled(JSContext *cx, bool enabled);

typedef enum JSCompilerOption {
    JSCOMPILER_BASELINE_USECOUNT_TRIGGER,
    JSCOMPILER_ION_USECOUNT_TRIGGER,
    JSCOMPILER_PJS_ENABLE
} JSCompilerOption;

extern JS_PUBLIC_API(void)
JS_SetGlobalCompilerOption(JSContext *cx, JSCompilerOption opt, uint32_t value);

/*
 * Convert a uint32_t index into a jsid.
 */
extern JS_PUBLIC_API(JSBool)
JS_IndexToId(JSContext *cx, uint32_t index, jsid *id);

/*
 * Convert chars into a jsid.
 *
 * |chars| may not be an index.
 */
extern JS_PUBLIC_API(JSBool)
JS_CharsToId(JSContext* cx, JS::TwoByteChars chars, jsid *idp);

/*
 *  Test if the given string is a valid ECMAScript identifier
 */
extern JS_PUBLIC_API(JSBool)
JS_IsIdentifier(JSContext *cx, JSString *str, JSBool *isIdentifier);

/*
 * Return the current script and line number of the most currently running
 * frame. Returns true if a scripted frame was found, false otherwise.
 */
extern JS_PUBLIC_API(JSBool)
JS_DescribeScriptedCaller(JSContext *cx, JSScript **script, unsigned *lineno);


/*
 * Encode/Decode interpreted scripts and functions to/from memory.
 */

extern JS_PUBLIC_API(void *)
JS_EncodeScript(JSContext *cx, JSScript *script, uint32_t *lengthp);

extern JS_PUBLIC_API(void *)
JS_EncodeInterpretedFunction(JSContext *cx, JSObject *funobj, uint32_t *lengthp);

extern JS_PUBLIC_API(JSScript *)
JS_DecodeScript(JSContext *cx, const void *data, uint32_t length,
                JSPrincipals *principals, JSPrincipals *originPrincipals);

extern JS_PUBLIC_API(JSObject *)
JS_DecodeInterpretedFunction(JSContext *cx, const void *data, uint32_t length,
                             JSPrincipals *principals, JSPrincipals *originPrincipals);

namespace JS {

extern JS_PUBLIC_DATA(const HandleValue) NullHandleValue;
extern JS_PUBLIC_DATA(const HandleValue) UndefinedHandleValue;

extern JS_PUBLIC_DATA(const HandleId) JSID_VOIDHANDLE;
extern JS_PUBLIC_DATA(const HandleId) JSID_EMPTYHANDLE;

} /* namespace JS */

namespace js {

/*
 * Import some JS:: names into the js namespace so we can make unqualified
 * references to them.
 */

using JS::Value;
using JS::IsPoisonedValue;
using JS::NullValue;
using JS::UndefinedValue;
using JS::Int32Value;
using JS::DoubleValue;
using JS::StringValue;
using JS::BooleanValue;
using JS::ObjectValue;
using JS::MagicValue;
using JS::NumberValue;
using JS::ObjectOrNullValue;
using JS::PrivateValue;
using JS::PrivateUint32Value;

using JS::IsPoisonedPtr;
using JS::IsPoisonedId;

using JS::StableCharPtr;
using JS::TwoByteChars;
using JS::Latin1CharsZ;

using JS::AutoIdVector;
using JS::AutoValueVector;
using JS::AutoObjectVector;
using JS::AutoFunctionVector;
using JS::AutoScriptVector;
using JS::AutoIdArray;

using JS::AutoGCRooter;
using JS::AutoArrayRooter;
using JS::AutoVectorRooter;
using JS::AutoHashMapRooter;
using JS::AutoHashSetRooter;

using JS::CallArgs;
using JS::IsAcceptableThis;
using JS::NativeImpl;
using JS::CallReceiver;
using JS::CompileOptions;
using JS::CallNonGenericMethod;

using JS::Rooted;
using JS::RootedObject;
using JS::RootedModule;
using JS::RootedFunction;
using JS::RootedScript;
using JS::RootedString;
using JS::RootedId;
using JS::RootedValue;

using JS::Handle;
using JS::HandleObject;
using JS::HandleModule;
using JS::HandleFunction;
using JS::HandleScript;
using JS::HandleString;
using JS::HandleId;
using JS::HandleValue;

using JS::MutableHandle;
using JS::MutableHandleObject;
using JS::MutableHandleFunction;
using JS::MutableHandleScript;
using JS::MutableHandleString;
using JS::MutableHandleId;
using JS::MutableHandleValue;

using JS::Zone;

} /* namespace js */

#endif /* jsapi_h */

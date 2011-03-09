/*
 * The MIT License
 *
 * Copyright (c) 2010 Sam Day
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "repository.h"
#include "commit.h"
#include "tree.h"
#include "index.h"
#include "tag.h"
#include "rev_walker.h"
#include "rawobj.h"
#include "ref.h"

// DANGER, WILL ROBINSON!
// The nastiest code that will ever rape your eyeballs follows.
// While writing async callbacks for getting/creating all the different objects
// I decided that no, I didn't want to copy and paste the same blocks of code
// 6 trillion times. Instead, I write a bunch of ugly motherfucking macros that
// I have no hope of maintaining in future. Fuck I'm rad.

#define LOAD_OID_ARG(I, VAR)												\
  if (args.Length() <= (I) || !args[I]->IsString()) 						\
	return ThrowException(Exception::TypeError(								\
				  String::New("Argument " #I " invalid")));					\
  if(git_oid_mkstr(&VAR, *(String::Utf8Value(args[I]->ToString()))) == GIT_ENOTOID) \
  	return ThrowException(Exception::TypeError(								\
  				  String::New("Argument " #I " is not an oid")));

#define GET_REQUEST_DATA(REQUESTTYPE)										\
	REQUESTTYPE *reqData =													\
		static_cast<REQUESTTYPE*>(req->data);

#define REQUEST_CLEANUP()													\
    reqData->callback.Dispose();											\
 	ev_unref(EV_DEFAULT_UC);												\
 	reqData->repo->Unref();													\
 	delete reqData;															\
	return 0;

#define REQUEST_DETACH(OBJ, FN, AFTERFN)									\
	OBJ->Ref();																\
	eio_custom(FN, EIO_PRI_DEFAULT, AFTERFN, request);						\
	ev_ref(EV_DEFAULT_UC);													\
	return scope.Close(Undefined());

#define CREATE_ASYNC_REQUEST(REQUESTCLASS)									\
	REQUESTCLASS *request = new REQUESTCLASS();								\
	request->callback = Persistent<Function>::New(callbackArg);				\
	request->repo = repo;

#define PREPARE_ASYNC_OID_GET(CLASS)										\
	REQ_FUN_ARG(args.Length() - 1, callbackArg);							\
	CREATE_ASYNC_REQUEST(get_oid_object_request);							\
	LOAD_OID_ARG(0, request->oid);											\
	REQUEST_DETACH(repo, EIO_Get##CLASS, EIO_Return##CLASS);				

#define PREPARE_ASYNC_NAMED_GET(CLASS, NAMEINDEX)							\
	REQ_FUN_ARG(args.Length() - 1, callbackArg);							\
	CREATE_ASYNC_REQUEST(get_named_object_request);							\
	request->name = new String::Utf8Value(args[NAMEINDEX]);					\
	REQUEST_DETACH(repo, EIO_Get##CLASS, EIO_Return##CLASS);

#define PREPARE_ASYNC_CREATE(CLASS)											\
	REQ_FUN_ARG(args.Length() - 1, callbackArg);							\
	CREATE_ASYNC_REQUEST(create_object_request)								\
	REQUEST_DETACH(repo, EIO_Create##CLASS, EIO_ReturnCreated##CLASS);

#define SETUP_CALLBACK_ARGS(TYPE, CLASS)									\
		Handle<Value> callbackArgs[2];										\
	 	if(reqData->error) {												\
	 		Handle<Value> error = CreateGitError(String::New(				\
	 				"Couldn't get " # CLASS), reqData->error);				\
	 		callbackArgs[0] = error;										\
	 		callbackArgs[1] = Null();										\
		}																	\
		else {																\
			CLASS *object = reqData->repo->wrap##CLASS(						\
					static_cast<TYPE*>(reqData->object));					\
			callbackArgs[0] = Null();										\
			callbackArgs[1] = object->handle_;								\
		}

#define ASYNC_GET_REPO_OID_OBJECT_FN(TYPE, CLASS)							\
	int Repository::EIO_Get##CLASS(eio_req *req) {							\
		GET_REQUEST_DATA(get_oid_object_request);							\
	 	TYPE* object;														\
	 	int result = reqData->error = reqData->repo->get##CLASS(			\
	 			&reqData->oid, &object);									\
		if(result == GIT_SUCCESS) {											\
			reqData->object = object;										\
		}																	\
		return 0;															\
	}

#define ASYNC_GET_REPO_NAMED_OBJECT_FN(TYPE, CLASS)							\
	int Repository::EIO_Get##CLASS(eio_req *req) {							\
		GET_REQUEST_DATA(get_named_object_request);							\
	 	TYPE* object;														\
	 	int result = reqData->error = reqData->repo->get##CLASS(			\
	 			**reqData->name, &object);									\
		if(result == GIT_SUCCESS) {											\
			reqData->object = object;										\
		}																	\
		return 0;															\
	}

#define ASYNC_CREATE_REPO_OBJECT_FN(TYPE, CLASS)							\
	int Repository::EIO_Create##CLASS(eio_req *req) {						\
		GET_REQUEST_DATA(create_object_request);							\
	 	TYPE* object;														\
		int result = reqData->error = reqData->repo->create##CLASS( 		\
				&object);													\
		if(result == GIT_SUCCESS) {											\
			reqData->object = object;										\
		}																	\
	}
	
#define ASYNC_RETURN_REPO_OID_OBJECT_FN(TYPE, CLASS)						\
	int Repository::EIO_Return##CLASS(eio_req *req) {						\
		HandleScope scope;													\
		GET_REQUEST_DATA(get_oid_object_request);							\
		SETUP_CALLBACK_ARGS(TYPE, CLASS);									\
		TRIGGER_CALLBACK();													\
	    REQUEST_CLEANUP();													\
	}

#define ASYNC_RETURN_REPO_NAMED_OBJECT_FN(TYPE, CLASS)						\
	int Repository::EIO_Return##CLASS(eio_req *req) {						\
		HandleScope scope;													\
		GET_REQUEST_DATA(get_named_object_request);							\
		SETUP_CALLBACK_ARGS(TYPE, CLASS);									\
		TRIGGER_CALLBACK();													\
		delete reqData->name;												\
	    REQUEST_CLEANUP();													\
	}

#define ASYNC_RETURN_REPO_CREATED_OBJECT_FN(TYPE, CLASS)					\
	int Repository::EIO_ReturnCreated##CLASS(eio_req *req) {				\
		HandleScope scope;													\
		GET_REQUEST_DATA(create_object_request);							\
		SETUP_CALLBACK_ARGS(TYPE, CLASS);									\
		TRIGGER_CALLBACK();													\
	    REQUEST_CLEANUP();													\
	}

namespace gitteh {

struct get_oid_object_request {
	Persistent<Function> callback;
	Repository *repo;
	int error;
	void *object;
	git_oid oid;
};

struct get_named_object_request {
	Persistent<Function> callback;
	Repository *repo;
	int error;
	void *object;
	String::Utf8Value *name;
};

struct create_object_request {
	Persistent<Function> callback;
	Repository *repo;
	int error;
	void *object;
};

struct create_symref_request {
	Persistent<Function> callback;
	Repository *repo;
	int error;
	git_reference *object;
	String::Utf8Value *name;
	String::Utf8Value *target;
};

Persistent<FunctionTemplate> Repository::constructor_template;

void Repository::Init(Handle<Object> target) {
	HandleScope scope;

	Local<FunctionTemplate> t = FunctionTemplate::New(New);
	constructor_template = Persistent<FunctionTemplate>::New(t);
	constructor_template->SetClassName(String::New("Repository"));
	t->InstanceTemplate()->SetInternalFieldCount(1);

	NODE_SET_PROTOTYPE_METHOD(t, "getCommit", GetCommit);
	NODE_SET_PROTOTYPE_METHOD(t, "getTree", GetTree);
	NODE_SET_PROTOTYPE_METHOD(t, "getTag", GetTag);
	NODE_SET_PROTOTYPE_METHOD(t, "getRawObject", GetRawObject);
	NODE_SET_PROTOTYPE_METHOD(t, "getReference", GetReference);

	NODE_SET_PROTOTYPE_METHOD(t, "createWalker", CreateWalker);
	NODE_SET_PROTOTYPE_METHOD(t, "createRawObject", CreateRawObject);
	NODE_SET_PROTOTYPE_METHOD(t, "createTag", CreateTag);
	NODE_SET_PROTOTYPE_METHOD(t, "createTree", CreateTree);
	NODE_SET_PROTOTYPE_METHOD(t, "createCommit", CreateCommit);
	NODE_SET_PROTOTYPE_METHOD(t, "createOidReference", CreateOidRef);
	NODE_SET_PROTOTYPE_METHOD(t, "createSymbolicReference", CreateSymbolicRef);

	NODE_SET_PROTOTYPE_METHOD(t, "exists", Exists);

	t->InstanceTemplate()->SetAccessor(String::New("index"), IndexGetter);

	target->Set(String::New("Repository"), t->GetFunction());
}

Handle<Value> Repository::New(const Arguments& args) {
	HandleScope scope;

	REQ_ARGS(1);
	REQ_STR_ARG(0, path);

	Repository *repo = new Repository();

	if(int result = git_repository_open(&repo->repo_, *path) != GIT_SUCCESS) {
		Handle<Value> ex = Exception::Error(String::New("Git error."));
		return ThrowException(ex);
	}

	repo->path_ = *path;

	args.This()->Set(String::New("path"), String::New(repo->path_), ReadOnly);

	repo->odb_ = git_repository_database(repo->repo_);

	repo->Wrap(args.This());
	return args.This();
}

Handle<Value> Repository::CreateCommit(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	if(args.Length() > 0) {
		PREPARE_ASYNC_CREATE(Commit);
	}
	else {
		git_commit *commit;
		int result = git_commit_new(&commit, repo->repo_);
	
		if(result != GIT_SUCCESS) {
			// TODO: error handling.
			return Null();
		}
	
		Commit *commitObject = repo->wrapCommit(commit);
		return scope.Close(commitObject->handle_);
	}
}

Handle<Value> Repository::GetCommit(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	REQ_ARGS(1);
	REQ_OID_ARG(0, oidArg);

	if(args.Length() == 2) {
		PREPARE_ASYNC_OID_GET(Commit);
	}
	else {
		Commit *commitObject;
		git_commit *commit;
		int res = repo->getCommit(&oidArg, &commit);
		if(res != GIT_SUCCESS) {
			THROW_GIT_ERROR("Couldn't get commit", res);
		}

		commitObject = repo->wrapCommit(commit);
		return scope.Close(commitObject->handle_);
	}
}

Handle<Value> Repository::CreateTree(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	if(args.Length() > 0) {
		PREPARE_ASYNC_CREATE(Tree);
	}
	else {
		git_tree *tree;
		int res = repo->createTree(&tree);
		if(res != GIT_SUCCESS)
			THROW_GIT_ERROR("Couldn't create tree.", res);
	
		Tree *treeObject = repo->wrapTree(tree);
		return treeObject->handle_;
	}
}

Handle<Value> Repository::GetTree(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	REQ_ARGS(1);
	REQ_OID_ARG(0, oidArg);

	if(args.Length() == 2) {
		PREPARE_ASYNC_OID_GET(Tree);
	}
	else {
		git_tree *tree;
		int res = repo->getTree(&oidArg, &tree);
		if(res != GIT_SUCCESS) {
			THROW_GIT_ERROR("Couldn't get Tree.", res);
		}

		Tree *treeObject = repo->wrapTree(tree);
		return scope.Close(treeObject->handle_);
	}
}

Handle<Value> Repository::CreateTag(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	if(args.Length() > 0) {
		PREPARE_ASYNC_CREATE(Tag);
	}
	else {
		git_tag *tag;
		int res = repo->createTag(&tag);
		if(res != GIT_SUCCESS)
			THROW_GIT_ERROR("Couldn't create new tag.", res);
	
		Tag *tagObject = repo->wrapTag(tag);
		return scope.Close(tagObject->handle_);
	}
}

Handle<Value> Repository::GetTag(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	REQ_ARGS(1);
	REQ_OID_ARG(0, oidArg);

	if(args.Length() == 2) {
		PREPARE_ASYNC_OID_GET(Tag);
	}
	else {
		git_tag *tag;
		int res = repo->getTag(&oidArg, &tag);
		if(res != GIT_SUCCESS) {
			THROW_GIT_ERROR("Couldn't get Tag.", res);
		}

		Tag *tagObject = repo->wrapTag(tag);
		return scope.Close(tagObject->handle_);
	}
}

Handle<Value> Repository::GetRawObject(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	REQ_ARGS(1);
	REQ_OID_ARG(0, oidArg);

	if(args.Length() == 2) {
		PREPARE_ASYNC_OID_GET(RawObject);
	}
	else {
		git_rawobj *obj;
		int res = repo->getRawObject(&oidArg, &obj);
		if(res != GIT_SUCCESS) {
			THROW_GIT_ERROR("Couldn't get RawObject.", res);
		}

		RawObject *objObj = repo->wrapRawObject(obj);
		return scope.Close(objObj->handle_);
	}
}

Handle<Value> Repository::CreateRawObject(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	if(args.Length() > 0) {
		PREPARE_ASYNC_CREATE(RawObject);
	}
	else {
		// Initialize a new rawobj.
		git_rawobj *rawObj;
		int res = repo->createRawObject(&rawObj);
		if(res != GIT_SUCCESS) {
			THROW_GIT_ERROR("Couldn't create RawObject.", res);
		}

		RawObject *objObj = repo->wrapRawObject(rawObj);
		return scope.Close(objObj->handle_);
	}
}

Handle<Value> Repository::CreateWalker(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	if(args.Length() > 0) {
		PREPARE_ASYNC_CREATE(RevWalker);
	}
	else {
		git_revwalk *walker;
		int res = repo->createRevWalker(&walker);
		if(res != GIT_SUCCESS) {
			THROW_GIT_ERROR("Couldn't create revision walker", res);
		}

		RevWalker *walkerObject = repo->wrapRevWalker(walker);
		return scope.Close(walkerObject->handle_);
	}
}

Handle<Value> Repository::IndexGetter(Local<String>, const AccessorInfo& info) {
	HandleScope scope;

	Repository *repo = ObjectWrap::Unwrap<Repository>(info.This());
	if(repo->index_ == NULL) {
		git_index *index;
		int result = git_repository_index(&index, repo->repo_);
		if(result == GIT_EBAREINDEX) {
			git_index_open_bare(&index, repo->path_);
		}

		Handle<Value> arg = External::New(index);
		Handle<Object> instance = Index::constructor_template->GetFunction()->NewInstance(1, &arg);
		repo->index_ = ObjectWrap::Unwrap<Index>(instance);
	}

	return repo->index_->handle_;
}

Handle<Value> Repository::GetReference(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	REQ_ARGS(1);

	if(args.Length() > 1) {
		PREPARE_ASYNC_NAMED_GET(Reference, 0);
	}
	else {
		REQ_STR_ARG(0, nameArg);
		git_reference *reference;
		int result = repo->getReference(*nameArg, &reference);
		if(result != GIT_SUCCESS) {
			THROW_GIT_ERROR("Failed to load ref.", result);
		}

		Reference *refObj = repo->wrapReference(reference);
		return scope.Close(refObj->handle_);
	}
}

Handle<Value> Repository::CreateSymbolicRef(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	REQ_ARGS(2);
	REQ_STR_ARG(0, nameArg);
	REQ_STR_ARG(1, targetArg);

	if(!nameArg.length()) {
		THROW_ERROR("Please provide a name.");
	}

	if(!targetArg.length()) {
		THROW_ERROR("Please provide a target for the symbolic ref.");
	}

	if(args.Length() > 2) {

	}
	else {
		git_reference *ref;
		int res = git_reference_create_symbolic(&ref, repo->repo_, *nameArg, *targetArg);

		if(res != GIT_SUCCESS) {
			THROW_GIT_ERROR("Couldn't create reference.", res);
		}

		Reference *refObj = repo->wrapReference(ref);
		return scope.Close(refObj->handle_);
	}
}

Handle<Value> Repository::CreateOidRef(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	REQ_ARGS(2);
	REQ_STR_ARG(0, nameArg);
	REQ_OID_ARG(1, oidArg);

	if(!nameArg.length()) {
		THROW_ERROR("Please provide a name.");
	}

	git_reference *ref;
	int res = git_reference_create_oid(&ref, repo->repo_, *nameArg, &oidArg);
	if(res != GIT_SUCCESS) {
		THROW_GIT_ERROR("Couldn't create reference.", res);
	}

	Reference *refObj = repo->wrapReference(ref);
	return scope.Close(refObj->handle_);
}

Handle<Value> Repository::Exists(const Arguments& args) {
	HandleScope scope;
	Repository *repo = ObjectWrap::Unwrap<Repository>(args.This());

	REQ_ARGS(1);
	REQ_OID_ARG(0, objOid);

	return Boolean::New(git_odb_exists(repo->odb_, &objOid));
}

// Boilerplate code can SMFD.
// ==========
// COMMIT EIO
// ==========
ASYNC_GET_REPO_OID_OBJECT_FN(git_commit, Commit)
ASYNC_RETURN_REPO_OID_OBJECT_FN(git_commit, Commit)
ASYNC_CREATE_REPO_OBJECT_FN(git_commit, Commit)
ASYNC_RETURN_REPO_CREATED_OBJECT_FN(git_commit, Commit)

// ========
// TREE EIO
// ========
ASYNC_GET_REPO_OID_OBJECT_FN(git_tree, Tree)
ASYNC_RETURN_REPO_OID_OBJECT_FN(git_tree, Tree)
ASYNC_CREATE_REPO_OBJECT_FN(git_tree, Tree)
ASYNC_RETURN_REPO_CREATED_OBJECT_FN(git_tree, Tree)

// =======
// TAG EIO
// =======
ASYNC_GET_REPO_OID_OBJECT_FN(git_tag, Tag)
ASYNC_RETURN_REPO_OID_OBJECT_FN(git_tag, Tag)
ASYNC_CREATE_REPO_OBJECT_FN(git_tag, Tag)
ASYNC_RETURN_REPO_CREATED_OBJECT_FN(git_tag, Tag)

// =============
// RAWOBJECT EIO
// =============
ASYNC_CREATE_REPO_OBJECT_FN(git_rawobj, RawObject)
ASYNC_RETURN_REPO_OID_OBJECT_FN(git_rawobj, RawObject)
ASYNC_GET_REPO_OID_OBJECT_FN(git_rawobj, RawObject)
ASYNC_RETURN_REPO_CREATED_OBJECT_FN(git_rawobj, RawObject)

// =======
// REF EIO
// =======
ASYNC_GET_REPO_NAMED_OBJECT_FN(git_reference, Reference)
ASYNC_RETURN_REPO_NAMED_OBJECT_FN(git_reference, Reference)

// ===========
// REVWALK EIO
// ===========
ASYNC_CREATE_REPO_OBJECT_FN(git_revwalk, RevWalker)
ASYNC_RETURN_REPO_CREATED_OBJECT_FN(git_revwalk, RevWalker)

Repository::Repository() {
	CREATE_MUTEX(gitLock_);
}

Repository::~Repository() {
	close();
}

void Repository::close() {
	if(repo_) {
		git_repository_free(repo_);
		repo_ = NULL;
	}
}

int Repository::getCommit(git_oid *id, git_commit **cmt) {
	int result;
	
	LOCK_MUTEX(gitLock_);
	result = git_commit_lookup(cmt, repo_, id);
	UNLOCK_MUTEX(gitLock_);
	
	return result;
}

int Repository::createCommit(git_commit **commit) {
	int result;
	
	LOCK_MUTEX(gitLock_);
	result = git_commit_new(commit, repo_);
	UNLOCK_MUTEX(gitLock_);

	return result;
}

Commit *Repository::wrapCommit(git_commit *commit) {
	Commit *commitObject;
	
	LOCK_MUTEX(gitLock_);
	if(commitStore_.getObjectFor(commit, &commitObject)) {
		// Commit needs to know who it's daddy is.
		commitObject->repository_ = this;
	}
	UNLOCK_MUTEX(gitLock_);

	return commitObject;
}

int Repository::createTree(git_tree **tree) {
	int result;
	
	LOCK_MUTEX(gitLock_);
	result = git_tree_new(tree, repo_);
	UNLOCK_MUTEX(gitLock_);
	
	return result;
}

int Repository::getTree(git_oid *id, git_tree **tree) {
	int result;
	
	LOCK_MUTEX(gitLock_);
	result = git_tree_lookup(tree, repo_, id);
	UNLOCK_MUTEX(gitLock_);
	
	return result;
}

Tree *Repository::wrapTree(git_tree *tree) {
	Tree *treeObject;
	if(treeStore_.getObjectFor(tree, &treeObject)) {
		treeObject->repository_ = this;
	}

	return treeObject;
}

int Repository::createTag(git_tag **tag) {
	int result;

	LOCK_MUTEX(gitLock_);
	result = git_tag_new(tag, repo_);
	UNLOCK_MUTEX(gitLock_);

	return result;
}

int Repository::getTag(git_oid *id, git_tag **tag) {
	int result;

	LOCK_MUTEX(gitLock_);
	result = git_tag_lookup(tag, repo_, id);
	UNLOCK_MUTEX(gitLock_);

	return result;
}

Tag *Repository::wrapTag(git_tag *tag) {
	Tag *tagObject;
	if(tagStore_.getObjectFor(tag, &tagObject)) {
		tagObject->repository_ = this;
	}

	return tagObject;
}

int Repository::getReference(char* name, git_reference** ref) {
	int result;

	LOCK_MUTEX(gitLock_);
	result = git_reference_lookup(ref, repo_, name);
	UNLOCK_MUTEX(gitLock_);

	return result;
}

Reference *Repository::wrapReference(git_reference *ref) {
	Reference *refObj;
	if(refStore_.getObjectFor(ref, &refObj)) {
		refObj->repository_ = this;
	}
	
	return refObj;
}

int Repository::getRawObject(git_oid *id, git_rawobj **objPtr) {
	int result;

	LOCK_MUTEX(gitLock_);
	git_rawobj *obj = new git_rawobj;
	result = git_odb_read(obj, odb_, id);
	UNLOCK_MUTEX(gitLock_);
	
	if(result == GIT_SUCCESS) {
		*objPtr = obj;
	}

	return result;
}

int Repository::createRevWalker(git_revwalk **walker) {
	int result;

	LOCK_MUTEX(gitLock_);
	result = git_revwalk_new(walker, repo_);
	UNLOCK_MUTEX(gitLock_);

	return result;
}

RevWalker *Repository::wrapRevWalker(git_revwalk *walker) {
	HandleScope scope;

	Handle<Value> constructorArgs[1] = { External::New(walker) };
	Handle<Object> jsObject = RevWalker::constructor_template->GetFunction()->NewInstance(1, constructorArgs);

	RevWalker *walkerObj = ObjectWrap::Unwrap<RevWalker>(jsObject);
	walkerObj->repo_ = this;

	return walkerObj;
}

RawObject *Repository::wrapRawObject(git_rawobj *rawObj) {
	HandleScope scope;

	Handle<Value> constructorArgs[1] = { External::New(rawObj) };
	Handle<Object> jsObject = RawObject::constructor_template->GetFunction()->NewInstance(1, constructorArgs);

	RawObject *rawObjObj = ObjectWrap::Unwrap<RawObject>(jsObject);
	rawObjObj->repository_ = this;

	return rawObjObj;
}

int Repository::createRawObject(git_rawobj** rawObj) {
	*rawObj = new git_rawobj;
	(*rawObj)->len = 0;
	(*rawObj)->type = GIT_OBJ_BAD;

	return GIT_SUCCESS;
}

git_commit* Repository::getParentCommit(git_commit* commit, int index) {
	git_commit *result;

	LOCK_MUTEX(gitLock_);
	result = git_commit_parent(commit, index);
	UNLOCK_MUTEX(gitLock_);

	return result;
}

} // namespace gitteh

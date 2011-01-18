#include "v8.h"
#include "mipspro-tplinst.h"

//
// What is happening here : during linking stage the mipspro linker fails
// in a template instantiation loop, so we specifiy all instantiations here.
// Because these instantiation occurs in this compilation unit, we had to move
// a lot of class definition back in their respective headers, and get rid of
// some anonymous namespaces
// We hope to get rid of this at some point
//


// Manual listing
#pragma instantiate void v8::internal::String::WriteToFlat(v8::internal::String*,unsigned short*,int,int)
#pragma instantiate void v8::internal::String::WriteToFlat(v8::internal::String*,char*,int,int)
#pragma instantiate v8::Local<v8::String> v8::HandleScope::Close(v8::Handle<v8::String>)
#pragma instantiate v8::Local<v8::Value> v8::HandleScope::Close(v8::Handle<v8::Value>)
#pragma instantiate v8::Local<v8::StackTrace> v8::HandleScope::Close(v8::Handle<v8::StackTrace>)
#pragma instantiate v8::Local<v8::StackFrame> v8::HandleScope::Close(v8::Handle<v8::StackFrame>)
#pragma instantiate v8::Local<v8::Array> v8::HandleScope::Close(v8::Handle<v8::Array>)
#pragma instantiate v8::Local<v8::Object> v8::HandleScope::Close(v8::Handle<v8::Object>)
#pragma instantiate v8::internal::StaticNewSpaceVisitor<v8::internal::NewSpaceScavenger>::table_
#pragma instantiate v8::internal::StaticNewSpaceVisitor<v8::internal::StaticPointersToNewGenUpdatingVisitor>::table_
#pragma instantiate bool v8::internal::DateParser::Parse<const char>(v8::internal::Vector<const char>,v8::internal::FixedArray*)
#pragma instantiate bool v8::internal::DateParser::Parse<const unsigned short>(v8::internal::Vector<const unsigned short>,v8::internal::FixedArray*)
#pragma instantiate void v8::internal::MarkCompactCollector::EncodeForwardingAddressesInPagedSpace<v8::internal::MCAllocateFromMapSpace,v8::internal::IgnoreNonLiveObject>(v8::internal::PagedSpace*)
#pragma instantiate void v8::internal::MarkCompactCollector::EncodeForwardingAddressesInPagedSpace<v8::internal::MCAllocateFromOldPointerSpace,v8::internal::IgnoreNonLiveObject>(v8::internal::PagedSpace*)
#pragma instantiate void v8::internal::MarkCompactCollector::EncodeForwardingAddressesInPagedSpace<v8::internal::MCAllocateFromOldDataSpace,v8::internal::IgnoreNonLiveObject>(v8::internal::PagedSpace*)
#pragma instantiate void v8::internal::MarkCompactCollector::EncodeForwardingAddressesInPagedSpace<v8::internal::MCAllocateFromCodeSpace,v8::internal::IgnoreNonLiveObject>(v8::internal::PagedSpace*)
#pragma instantiate void v8::internal::MarkCompactCollector::EncodeForwardingAddressesInPagedSpace<v8::internal::MCAllocateFromCellSpace,v8::internal::IgnoreNonLiveObject>(v8::internal::PagedSpace*)
#pragma instantiate void v8::internal::MarkCompactCollector::EncodeForwardingAddressesInPagedSpace<v8::internal::MCAllocateFromOldPointerSpace,v8::internal::MarkCompactCollector::ReportDeleteIfNeeded>(v8::internal::PagedSpace*)
#pragma instantiate void v8::internal::MarkCompactCollector::EncodeForwardingAddressesInPagedSpace<v8::internal::MCAllocateFromCodeSpace,v8::internal::MarkCompactCollector::ReportDeleteIfNeeded>(v8::internal::PagedSpace*)
#pragma instantiate void v8::internal::FindStringIndices<const char,const char>(v8::internal::Vector<const char>,v8::internal::Vector<const char>,v8::internal::ZoneList<int>*,unsigned int)
#pragma instantiate void v8::internal::FindStringIndices<const unsigned short,const char>(v8::internal::Vector<const unsigned short>,v8::internal::Vector<const char>,v8::internal::ZoneList<int>*,unsigned int)
#pragma instantiate void v8::internal::FindStringIndices<const char,const unsigned short>(v8::internal::Vector<const char>,v8::internal::Vector<const unsigned short>,v8::internal::ZoneList<int>*,unsigned int)
#pragma instantiate void v8::internal::FindStringIndices<const unsigned short,const unsigned short>(v8::internal::Vector<const unsigned short>,v8::internal::Vector<const unsigned short>,v8::internal::ZoneList<int>*,unsigned int)
#pragma instantiate void v8::internal::CompiledReplacement::ParseReplacementPattern<const char>(v8::internal::ZoneList<v8::internal::CompiledReplacement::ReplacementPart>*,v8::internal::Vector<const char>,int,int)
#pragma instantiate void v8::internal::CompiledReplacement::ParseReplacementPattern<const unsigned short>(v8::internal::ZoneList<v8::internal::CompiledReplacement::ReplacementPart>*,v8::internal::Vector<const unsigned short>,int,int)


// Pass 1
#pragma instantiate unibrow::InputBuffer<unibrow::Utf8,unibrow::Buffer<const char*>,(const unsigned int)1024>::FillBuffer
#pragma instantiate unibrow::InputBuffer<unibrow::Utf8,unibrow::Buffer<const char*>,(const unsigned int)1024>::Rewind
#pragma instantiate unibrow::InputBuffer<unibrow::Utf8,unibrow::Buffer<const char*>,(const unsigned int)1024>::Seek
#pragma instantiate unibrow::InputBuffer<unibrow::Utf8,unibrow::Buffer<const char*>,(const unsigned int)256>::FillBuffer
#pragma instantiate unibrow::InputBuffer<unibrow::Utf8,unibrow::Buffer<const char*>,(const unsigned int)256>::Rewind
#pragma instantiate unibrow::InputBuffer<unibrow::Utf8,unibrow::Buffer<const char*>,(const unsigned int)256>::Seek
#pragma instantiate unibrow::InputBuffer<v8::internal::String,v8::internal::String**,(const unsigned int)256>::FillBuffer
#pragma instantiate unibrow::InputBuffer<v8::internal::String,v8::internal::String**,(const unsigned int)256>::Rewind
#pragma instantiate unibrow::InputBuffer<v8::internal::String,v8::internal::String*,(const unsigned int)1024>::FillBuffer
#pragma instantiate unibrow::InputBuffer<v8::internal::String,v8::internal::String*,(const unsigned int)1024>::Rewind
#pragma instantiate unibrow::Mapping<unibrow::CanonicalizationRange,(const int)256>::CalculateValue
#pragma instantiate unibrow::Mapping<unibrow::Ecma262Canonicalize,(const int)256>::CalculateValue
#pragma instantiate unibrow::Mapping<unibrow::Ecma262UnCanonicalize,(const int)256>::CalculateValue
#pragma instantiate unibrow::Mapping<unibrow::ToLowercase,(const int)128>::CalculateValue
#pragma instantiate unibrow::Mapping<unibrow::ToUppercase,(const int)128>::CalculateValue
#pragma instantiate unibrow::Predicate<unibrow::LineTerminator,(const int)128>::CalculateValue
#pragma instantiate unibrow::Predicate<unibrow::WhiteSpace,(const int)128>::CalculateValue
#pragma instantiate unibrow::Predicate<v8::internal::IdentifierPart,(const int)128>::CalculateValue
#pragma instantiate unibrow::Predicate<v8::internal::IdentifierStart,(const int)128>::CalculateValue
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::Add
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::Allocate
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::AtPut
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::CopyValuesTo
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::DeleteProperty
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::NumberOfElementsFilterAttributes
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::SlowReverseLookup
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::Add
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::Allocate
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::DeleteProperty
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::GenerateNewEnumerationIndices
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::NumberOfElementsFilterAttributes
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::NumberOfEnumElements
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::Print
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::SlowReverseLookup
#pragma instantiate v8::internal::ExternalStringUTF16Buffer<v8::internal::ExternalAsciiString,char>::ExternalStringUTF16Buffer
#pragma instantiate v8::internal::ExternalStringUTF16Buffer<v8::internal::ExternalAsciiString,char>::Initialize
#pragma instantiate v8::internal::ExternalStringUTF16Buffer<v8::internal::ExternalTwoByteString,unsigned short>::ExternalStringUTF16Buffer
#pragma instantiate v8::internal::ExternalStringUTF16Buffer<v8::internal::ExternalTwoByteString,unsigned short>::Initialize
#pragma instantiate v8::internal::HashTable<v8::internal::CodeCacheHashTableShape,v8::internal::HashTableKey*>::Allocate
#pragma instantiate v8::internal::HashTable<v8::internal::CodeCacheHashTableShape,v8::internal::HashTableKey*>::EnsureCapacity
#pragma instantiate v8::internal::HashTable<v8::internal::CodeCacheHashTableShape,v8::internal::HashTableKey*>::FindEntry
#pragma instantiate v8::internal::HashTable<v8::internal::CodeCacheHashTableShape,v8::internal::HashTableKey*>::FindInsertionEntry
#pragma instantiate v8::internal::HashTable<v8::internal::CompilationCacheShape,v8::internal::HashTableKey*>::Allocate
#pragma instantiate v8::internal::HashTable<v8::internal::CompilationCacheShape,v8::internal::HashTableKey*>::EnsureCapacity
#pragma instantiate v8::internal::HashTable<v8::internal::CompilationCacheShape,v8::internal::HashTableKey*>::FindEntry
#pragma instantiate v8::internal::HashTable<v8::internal::CompilationCacheShape,v8::internal::HashTableKey*>::FindInsertionEntry
#pragma instantiate v8::internal::HashTable<v8::internal::MapCacheShape,v8::internal::HashTableKey*>::Allocate
#pragma instantiate v8::internal::HashTable<v8::internal::MapCacheShape,v8::internal::HashTableKey*>::EnsureCapacity
#pragma instantiate v8::internal::HashTable<v8::internal::MapCacheShape,v8::internal::HashTableKey*>::FindEntry
#pragma instantiate v8::internal::HashTable<v8::internal::MapCacheShape,v8::internal::HashTableKey*>::FindInsertionEntry
#pragma instantiate v8::internal::HashTable<v8::internal::NumberDictionaryShape,unsigned int>::FindEntry
#pragma instantiate v8::internal::HashTable<v8::internal::StringDictionaryShape,v8::internal::String*>::FindEntry
#pragma instantiate v8::internal::HashTable<v8::internal::SymbolTableShape,v8::internal::HashTableKey*>::Allocate
#pragma instantiate v8::internal::HashTable<v8::internal::SymbolTableShape,v8::internal::HashTableKey*>::EnsureCapacity
#pragma instantiate v8::internal::HashTable<v8::internal::SymbolTableShape,v8::internal::HashTableKey*>::FindEntry
#pragma instantiate v8::internal::HashTable<v8::internal::SymbolTableShape,v8::internal::HashTableKey*>::FindInsertionEntry
#pragma instantiate v8::internal::List<char*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<const char*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<int,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<int,v8::internal::FreeStoreAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<int,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<int,v8::internal::ZoneListAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<long,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<unsigned char*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<unsigned char*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<unsigned int,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<unsigned int,v8::internal::ZoneListAllocationPolicy>::Contains
#pragma instantiate v8::internal::List<unsigned short,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::AlternativeGeneration*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Assembler::Trampoline,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::BreakTarget*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::CaseClause*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::CharacterRange,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::CharacterRange,v8::internal::ZoneListAllocationPolicy>::AddAll
#pragma instantiate v8::internal::List<v8::internal::CodeRange::FreeBlock,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::CodeRange::FreeBlock,v8::internal::FreeStoreAllocationPolicy>::AddAll
#pragma instantiate v8::internal::List<v8::internal::Context*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Context*,v8::internal::FreeStoreAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::Declaration*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::DeferredCode*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::DeferredCode*,v8::internal::FreeStoreAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::Expression*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::ExternalReferenceTable::ExternalReferenceEntry,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::FunctionLiteral*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Guard*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::GuardedAlternative,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::JSObject>,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::Object>,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::Object>,v8::internal::FreeStoreAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::Object>,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::PreallocatedStorage>::Add
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Heap::GCEpilogueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Heap::GCEpilogueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::Contains
#pragma instantiate v8::internal::List<v8::internal::Heap::GCEpilogueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::Heap::GCPrologueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Heap::GCPrologueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::Contains
#pragma instantiate v8::internal::List<v8::internal::Heap::GCPrologueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::HeapObject*,v8::internal::PreallocatedStorage>::Add
#pragma instantiate v8::internal::List<v8::internal::MemoryAllocator::ChunkInfo,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Object**,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Object**,v8::internal::FreeStoreAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::Object**,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Object*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Object*,v8::internal::FreeStoreAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::ObjectGroup*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::ObjectLiteral::Property*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::OutSet*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::RegExpCapture*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::RegExpNode*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::RegExpNode*,v8::internal::FreeStoreAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::RegExpNode*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::RegExpTree*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::RegExpTree*,v8::internal::ZoneListAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::RelocInfo,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::RelocInfo::Mode,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Scope*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::ShadowTarget*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::StackFrame*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Statement*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::TextElement,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::PreallocatedStorage>::Add
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::PreallocatedStorage>::Add
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::VariableProxy*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::VariableProxy*,v8::internal::ZoneListAllocationPolicy>::Remove
#pragma instantiate v8::internal::List<v8::internal::Vector<char>,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::Vector<unsigned int>,v8::internal::FreeStoreAllocationPolicy>::Add
#pragma instantiate v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::FindGreatestLessThan
#pragma instantiate v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::FindLeastGreaterThan
#pragma instantiate v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::Insert
#pragma instantiate v8::internal::ZoneSplayTree<v8::internal::DispatchTable::Config>::~ZoneSplayTree

#pragma instantiate void v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::CopyKeysTo(v8::internal::FixedArray*)
#pragma instantiate void v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::CopyKeysTo(v8::internal::FixedArray*,PropertyAttributes)
#pragma instantiate void v8::internal::JavaScriptFrameIteratorTemp<v8::internal::StackFrameIterator>::Advance(void)
#pragma instantiate void v8::internal::HashTable<v8::internal::SymbolTableShape,v8::internal::HashTableKey*>::IterateElements(v8::internal::ObjectVisitor*)
#pragma instantiate void v8::internal::List<v8::internal::CodeRange::FreeBlock,v8::internal::FreeStoreAllocationPolicy>::Sort(int (*)(const v8::internal::CodeRange::FreeBlock*,const v8::internal::CodeRange::FreeBlock*))
#pragma instantiate void v8::internal::List<v8::internal::Variable*,v8::internal::PreallocatedStorage>::Sort(int (*)(v8::internal::Variable *const*,v8::internal::Variable *const*))
#pragma instantiate void v8::internal::List<v8::internal::Variable*,v8::internal::ZoneListAllocationPolicy>::Sort(int (*)(v8::internal::Variable *const*,v8::internal::Variable *const*))
#pragma instantiate void v8::internal::List<v8::internal::Variable*,v8::internal::FreeStoreAllocationPolicy>::Sort(int (*)(v8::internal::Variable *const*,v8::internal::Variable *const*))
#pragma instantiate void v8::internal::JavaScriptFrameIteratorTemp<v8::internal::StackFrameIterator>::AdvanceToArgumentsFrame(void)
#pragma instantiate v8::internal::JavaScriptFrameIteratorTemp<v8::internal::StackFrameIterator>::JavaScriptFrameIteratorTemp(v8::internal::StackFrame::Id)
#pragma instantiate void v8::internal::HashTable<v8::internal::SymbolTableShape,v8::internal::HashTableKey*>::IteratePrefix(v8::internal::ObjectVisitor*)
#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEach(v8::internal::DispatchTableDumper*)
#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEach(v8::internal::TableEntryHeaderPrinter*)
#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEach(v8::internal::TableEntryBodyPrinter*)
#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEach(v8::internal::CharacterRangeSplitter*)
#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEach(v8::internal::AddDispatchRange*)
#pragma instantiate void v8::internal::List<v8::internal::CharacterRange,v8::internal::ZoneListAllocationPolicy>::Sort(int (*)(const v8::internal::CharacterRange*,const v8::internal::CharacterRange*))
#pragma instantiate void v8::internal::HashTable<v8::internal::CompilationCacheShape,v8::internal::HashTableKey*>::IterateElements(v8::internal::ObjectVisitor*)

typedef int (*mytype1)(v8::internal::String*,int,const unsigned char*,const unsigned char*,int*,unsigned char*,int);
#pragma instantiate mytype1 v8::internal::FUNCTION_CAST<mytype1>(v8::internal::Address)
typedef v8::Handle<v8::Value> (*mytype2)(v8::Local<v8::String>,const v8::AccessorInfo&);
#pragma instantiate mytype2 v8::internal::FUNCTION_CAST<mytype2>(v8::internal::Address)
typedef v8::internal::Object* (*mytype3)(unsigned char*,v8::internal::Object*,v8::internal::Object*,int,v8::internal::Object***);
#pragma instantiate mytype3 v8::internal::FUNCTION_CAST<mytype3>(v8::internal::Address)
typedef void (*mytype4)(const v8::Debug::EventDetails&);
#pragma instantiate mytype4 v8::internal::FUNCTION_CAST<mytype4>(v8::internal::Address)
typedef void (*mytype5)(v8::Handle<v8::Message>,v8::Handle<v8::Value>);
#pragma instantiate mytype5 v8::internal::FUNCTION_CAST<mytype5>(v8::internal::Address)
typedef void (*mytype6)(v8::Local<v8::String>,v8::Local<v8::Value>,const v8::AccessorInfo&);
#pragma instantiate mytype6 v8::internal::FUNCTION_CAST<mytype6>(v8::internal::Address)
typedef void (*mytype7)(v8::internal::MacroAssembler*,int,v8::internal::BuiltinExtraArguments);
#pragma instantiate mytype7 v8::internal::FUNCTION_CAST<mytype7>(v8::internal::Address)


// Pass 2
#pragma instantiate unibrow::InputBuffer<v8::internal::String,v8::internal::String**,(const unsigned int)256>::Seek
#pragma instantiate unibrow::InputBuffer<v8::internal::String,v8::internal::String*,(const unsigned int)1024>::Seek
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::AddEntry
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::EnsureCapacity
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::NumberOfEnumElements
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::AddEntry
#pragma instantiate v8::internal::Dictionary<v8::internal::StringDictionaryShape,v8::internal::String*>::EnsureCapacity
#pragma instantiate v8::internal::HashTable<v8::internal::NumberDictionaryShape,unsigned int>::Allocate
#pragma instantiate v8::internal::HashTable<v8::internal::StringDictionaryShape,v8::internal::String*>::Allocate
#pragma instantiate v8::internal::List<char*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<const char*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<int,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<int,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<long,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<unsigned char*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<unsigned char*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<unsigned int,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<unsigned short,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::AlternativeGeneration*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Assembler::Trampoline,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::BreakTarget*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::CaseClause*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::CharacterRange,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::CharacterRange,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::CodeRange::FreeBlock,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::CodeRange::FreeBlock,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Context*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Declaration*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::DeferredCode*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Expression*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::ExternalReferenceTable::ExternalReferenceEntry,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::FunctionLiteral*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Guard*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::GuardedAlternative,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::JSObject>,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::Object>,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::Object>,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::PreallocatedStorage>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Heap::GCEpilogueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Heap::GCPrologueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::HeapObject*,v8::internal::PreallocatedStorage>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::MemoryAllocator::ChunkInfo,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Object**,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Object**,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Object*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::ObjectGroup*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::ObjectLiteral::Property*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::OutSet*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::RegExpCapture*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::RegExpNode*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::RegExpNode*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::RegExpTree*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::RelocInfo,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::RelocInfo::Mode,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Scope*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::ShadowTarget*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::StackFrame*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Statement*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::TextElement,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::PreallocatedStorage>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::PreallocatedStorage>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::VariableProxy*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Vector<char>,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::Vector<unsigned int>,v8::internal::FreeStoreAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::FindGreatest
#pragma instantiate v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::FindLeast
#pragma instantiate v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::InsertInternal
#pragma instantiate v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::Splay
#pragma instantiate v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::~SplayTree

#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEachNode(v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::NodeToPairAdaptor<v8::internal::DispatchTableDumper>*)
#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEachNode(v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::NodeToPairAdaptor<v8::internal::TableEntryHeaderPrinter>*)
#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEachNode(v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::NodeToPairAdaptor<v8::internal::TableEntryBodyPrinter>*)
#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEachNode(v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::NodeToPairAdaptor<v8::internal::CharacterRangeSplitter>*)
#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEachNode(v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::NodeToPairAdaptor<v8::internal::AddDispatchRange>*)


// Pass 3
#pragma instantiate v8::internal::Dictionary<v8::internal::NumberDictionaryShape,unsigned int>::GenerateNewEnumerationIndices
#pragma instantiate v8::internal::HashTable<v8::internal::NumberDictionaryShape,unsigned int>::EnsureCapacity
#pragma instantiate v8::internal::HashTable<v8::internal::NumberDictionaryShape,unsigned int>::FindInsertionEntry
#pragma instantiate v8::internal::HashTable<v8::internal::StringDictionaryShape,v8::internal::String*>::EnsureCapacity
#pragma instantiate v8::internal::HashTable<v8::internal::StringDictionaryShape,v8::internal::String*>::FindInsertionEntry
#pragma instantiate v8::internal::List<char*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<const char*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<int,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<int,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<long,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<unsigned char*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<unsigned char*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<unsigned int,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<unsigned short,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::AlternativeGeneration*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Assembler::Trampoline,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::BreakTarget*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::CaseClause*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::CharacterRange,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::CodeRange::FreeBlock,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Context*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Declaration*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::DeferredCode*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Expression*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::ExternalReferenceTable::ExternalReferenceEntry,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::FunctionLiteral*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Guard*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::GuardedAlternative,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::JSObject>,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::Object>,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::Object>,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::PreallocatedStorage>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Heap::GCEpilogueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Heap::GCPrologueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::HeapObject*,v8::internal::PreallocatedStorage>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::MemoryAllocator::ChunkInfo,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Object**,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Object**,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Object*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::ObjectGroup*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::ObjectLiteral::Property*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::OutSet*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::RegExpCapture*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::RegExpNode*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::RegExpNode*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::RegExpTree*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::RelocInfo,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::RelocInfo::Mode,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Scope*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::ShadowTarget*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::Node*,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::StackFrame*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Statement*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::TextElement,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::PreallocatedStorage>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::PreallocatedStorage>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::VariableProxy*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Vector<char>,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::Vector<unsigned int>,v8::internal::FreeStoreAllocationPolicy>::ResizeAddInternal

#pragma instantiate void v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::ForEachNode(v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::NodeDeleter*)


// Pass 4
#pragma instantiate v8::internal::List<char*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<const char*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<int,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<int,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<long,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<unsigned char*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<unsigned char*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<unsigned int,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<unsigned short,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::AlternativeGeneration*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Assembler::Trampoline,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::BreakTarget*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::CaseClause*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Context*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Declaration*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::DeferredCode*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Expression*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::ExternalReferenceTable::ExternalReferenceEntry,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::FunctionLiteral*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Guard*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::GuardedAlternative,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::JSObject>,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::Object>,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::Object>,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::PreallocatedStorage>::Resize
#pragma instantiate v8::internal::List<v8::internal::Handle<v8::internal::String>,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Heap::GCEpilogueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Heap::GCPrologueCallbackPair,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::HeapObject*,v8::internal::PreallocatedStorage>::Resize
#pragma instantiate v8::internal::List<v8::internal::MemoryAllocator::ChunkInfo,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Object**,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Object**,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Object*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::ObjectGroup*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::ObjectLiteral::Property*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::OutSet*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::RegExpCapture*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::RegExpNode*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::RegExpNode*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::RegExpTree*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::RelocInfo,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::RelocInfo::Mode,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Scope*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::ShadowTarget*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::Node*,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::StackFrame*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Statement*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::TextElement,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::PreallocatedStorage>::Resize
#pragma instantiate v8::internal::List<v8::internal::Variable*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::PreallocatedStorage>::Resize
#pragma instantiate v8::internal::List<v8::internal::Variable::Mode,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::VariableProxy*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Vector<char>,v8::internal::FreeStoreAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::Vector<unsigned int>,v8::internal::FreeStoreAllocationPolicy>::Resize


// Pass 5
#pragma instantiate v8::internal::List<v8::internal::CompiledReplacement::ReplacementPart,v8::internal::ZoneListAllocationPolicy>::Add
#pragma instantiate v8::internal::List<v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::Node*,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::CompiledReplacement::ReplacementPart,v8::internal::ZoneListAllocationPolicy>::ResizeAdd
#pragma instantiate v8::internal::List<v8::internal::SplayTree<v8::internal::DispatchTable::Config,v8::internal::ZoneListAllocationPolicy>::Node*,v8::internal::ZoneListAllocationPolicy>::Resize
#pragma instantiate v8::internal::List<v8::internal::CompiledReplacement::ReplacementPart,v8::internal::ZoneListAllocationPolicy>::ResizeAddInternal
#pragma instantiate v8::internal::List<v8::internal::CompiledReplacement::ReplacementPart,v8::internal::ZoneListAllocationPolicy>::Resize

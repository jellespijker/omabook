#pragma once

#include <QtTypes>

class Library;

// What a book takes back from a tenant it leaves (ADR 0003, #97): moving a
// book changes membership, not data, and only the membership the book gave.
// Its facts and the relations learned in it leave the tenant; a thing it
// taught leaves too, unless another of the tenant's books is still behind it,
// and with it go its facts of no book and the relations that were there only
// because both ends were. What other books gave stays where it is.
//
// It is the rules of TenancySchema again, for the rows the book was behind
// and that one tenant, so it costs what the book taught, not the library;
// Tenancy::rebuild, which recomputes everything, is kept for the choice of
// one tenant for the whole library.
namespace TenancyLeave {

// Takes `book` out of `tenant` (both by their numbers here), and what it alone
// gave there. Inside the caller's batch.
void take(const Library &library, qint64 tenant, qint64 book);

} // namespace TenancyLeave

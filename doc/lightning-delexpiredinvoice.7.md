lightning-delexpiredinvoice -- Command for removing expired invoices
====================================================================

SYNOPSIS
--------

**delexpiredinvoice** \[*maxexpirytime*\]

DESCRIPTION
-----------

The **delexpiredinvoice** RPC command removes all invoices that have
expired on or before the given *maxexpirytime*.

If *maxexpirytime* is not specified then all expired invoices are
deleted.

RETURN VALUE
------------

On success, an empty object is returned.

AUTHOR
------

ZmnSCPxj <<ZmnSCPxj@protonmail.com>> is mainly responsible.

SEE ALSO
--------

lightning-delinvoice(7), lightning-autocleaninvoice(7)

RESOURCES
---------

Main web site: <https://github.com/ElementsProject/lightning>

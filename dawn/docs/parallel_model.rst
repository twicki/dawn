
Formal Definition of the Dawn Parallel Model
============================================

Contract to the user from HIR
-----------------------------

Dawn will generate code based on the operations passed from high-level
intermediate representation. It's specification can be found `here`_.

HIR execution model
~~~~~~~~~~~~~~~~~~~

Generally, stencil computations in dawn differentiates between
horizontal (denoted by `i,j`) and the vertical dimension (`k`).
The execution model is described by a `vertical_region` spanning
a k-range with direction which contains statements.

.. code:: c++

   1 vertical_region(k_start, k_end) // forward
   2   statement1
   3   statement2
   4 vertical_region(k_end, k_start) // backward
   5   statement3
   6   statement4

The HIR code as described by these constructs behaves as if

- *vertical_regions* are executed sequentially in the order they appear,
- *statements* inside *vertical_regions* are executed as (sequential) for-loops
  over the k-range,
- a *statement* inside the *vertical_region* is executed as a
  parallel for-loop over the horizontal dimension(s).

The pseudo-code translation of the above example is


.. code:: c++

   1  for k = k_start:k_end
   2    parfor ij
   3      statement1
   4    parfor ij
   5      statement2
   6  for k = k_end:k_start
   7    parfor ij
   8      statement3
   9    parfor ij
   10     statement4

where `parfor ij` means that there is no guarantee of the order in which horizontal points are executed.

Internal Contract of Data Structures
------------------------------------

StencilInstantiation
~~~~~~~~~~~~~~~~~~~~

StencilInstantiations (SI) are user defined and are never changed during
any optimization. Each SI will be globally split from every other one
and there is no optimization across SIs. For every SI, a callable API
will be generated.

Stencil
~~~~~~~

Stencils serve no purpose right now

--------------

   It might be useful to remove them altogether since even if they
   existed, no concept can't be captured by the other structures

--------------

Multistage
~~~~~~~~~~

A Multistage (MS) is the equivalent of a global synchronization between
all processes on the full domain. each Multistage can only have one loop
order

-  in the CUDA back end they are the equivalent of a kernel call
-  in the GridTools back end they are the equivalent of a multistage
-  in the naive back end every storage will be synchronized on the host
   at the beginning of each multistage

--------------

   Can we discuss why we still have the loop order on this level? Would
   it potentially be interesting to promote this to the Do level and
   change the fusion strategy if we use the GridTools back end? Or do we
   know that we are losing performance in the CUDA back end if we mix
   forward and backward loops versus new kernel calls?

--------------

Stage
~~~~~

A Stage (St) specifies a specific compute-domain for different
computations. These can be either global computation bounds or extended
compute domains due to larger required input data.

Each stage has the option to require (block-wide) synchronization.

-  in the CUDA back end they are equivalent to conditionals that check
   if the block / thread combination is inside the compute domain and a
   potential ``__syncthreads()`` afterwards.
-  in the GridTools back end they are the equivalent of a
   ``stage(_with_extents)``
-  in the naive back end they are represented a the innermost ij loops

--------------

   currently the ij loop of the stage is innermost, would it be
   interesting to move it to be the outermost one to be consistent with
   the execution model?

--------------

DoMethod
~~~~~~~~
A DoMethod (Do) specifies a vertical iteration space in which
computation happens. Dos in a St can't be overlapping.

-  in the CUDA back end each Do is represented with a for loop inside a
   kernel call
-  in the GridTools back end each Do is translated to an
   ``apply``\ function that has it's own interval specified
-  in the naive back end each Do is represented with an (outermost) ij
   loop

--------------

   Is the constraint that two Dos can't be overlapping in a Stage still
   useful for general back ends? Should the GT back end run its own pass
   to enforce this the same way the CUDA one does?

--------------

Execution Model from Internal Structures
----------------------------------------
The above assumptions require the following execution order:

.. code:: c++

   1 for MultiStage
   2  for k
   3   for Stage
   4    for ij
   5     for DoMethod
   6      execute_statements()

Since MS can be dependent on each other they have to be a sequential loop.
The assumption that each stage will have all it's dependent statements executed
on each lower k-level can be translated into the k-loop happening before the
Stage loop.

--------------

   The biggest question we have to ask ourselves is if we want to
   support multiple stages in a sequential k-setting. If so we need to
   be very precise here which we are not (yet). Depending on required
   performance, we want the ij loop to be as far outside as possible

--------------


   .. _here: https://github.com/MeteoSwiss-APN/HIR

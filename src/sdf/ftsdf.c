
#include <freetype/internal/ftobjs.h>
#include <freetype/internal/ftdebug.h>
#include <freetype/ftlist.h>
#include "ftsdf.h"

#include "ftsdferrs.h"

  /**************************************************************************
   *
   * macros
   *
   */

  #define FT_INT_26D6( x )   ( x * 64 )     /* convert int to 26.6 fixed point   */
  #define FT_INT_16D16( x )  ( x * 65536 )  /* convert int to 16.16 fixed point  */
  #define FT_26D6_16D16( x ) ( x * 1024 )   /* convert 26.6 to 16.16 fixed point */


  /* Convenient macro which calls the function */
  /* and returns if any error occurs.          */
  #define FT_CALL( x ) do                          \
                       {                           \
                         error = ( x );            \
                         if ( error != FT_Err_Ok ) \
                           goto Exit;              \
                       } while ( 0 )

  /**************************************************************************
   *
   * typedefs
   *
   */

  typedef  FT_Vector FT_26D6_Vec;   /* with 26.6 fixed point components  */
  typedef  FT_Vector FT_16D16_Vec;  /* with 16.16 fixed point components */

  typedef  FT_Fixed  FT_16D16;      /* 16.16 fixed point representation  */
  typedef  FT_Fixed  FT_26D6;       /* 26.6 fixed point representation   */

  /**************************************************************************
   *
   * structures and enums
   *
   */

  typedef struct  SDF_TRaster_
  {
    FT_Memory  memory; /* used internally to allocate memory */

  } SDF_TRaster;

  /* enumeration of all the types of curve present in vector fonts */
  typedef enum  SDF_Edge_Type_
  {
    SDF_EDGE_UNDEFINED  = 0,  /* undefined, used to initialize */
    SDF_EDGE_LINE       = 1,  /* straight line segment         */
    SDF_EDGE_CONIC      = 2,  /* second order bezier curve     */
    SDF_EDGE_CUBIC      = 3   /* third order bezier curve      */

  } SDF_Edge_Type;

  /* represent a single edge in a contour */
  typedef struct  SDF_Edge_
  {
    FT_26D6_Vec    start_pos;   /* start position of the edge             */
    FT_26D6_Vec    end_pos;     /* end position of the edge               */
    FT_26D6_Vec    control_a;   /* first control point of a bezier curve  */
    FT_26D6_Vec    control_b;   /* second control point of a bezier curve */

    SDF_Edge_Type  edge_type;   /* edge identifier                        */

  } SDF_Edge;

  /* A contour represent a set of edges which make a closed */
  /* loop.                                                  */
  typedef struct  SDF_Contour_
  {
    FT_26D6_Vec  last_pos;  /* end position of the last edge    */
    FT_ListRec   edges;     /* list of all edges in the contour */

  } SDF_Contour;

  /* Represent a set a contours which makes up a complete */
  /* glyph outline.                                       */
  typedef struct  SDF_Shape_
  {
    FT_Memory   memory;    /* used internally to allocate memory  */
    FT_ListRec  contours;  /* list of all contours in the outline */

  } SDF_Shape;

  typedef struct SDF_Signed_Distance_
  {
    /* Nearest point the outline to a given point.    */
    /* [note]: This is not a *direction* vector, this */
    /*         simply a *point* vector on the grid.   */
    FT_16D16_Vec  neartest_point;

    /* The normalized direction of the curve at the   */
    /* above point.                                   */
    /* [note]: This is a *direction* vector.          */
    FT_16D16_Vec  direction;

    /* Unsigned shortest squared distance from the    */
    /* point to the above `nearest_point'.            */
    FT_16D16      squared_distance;

    /* Represent weather the `nearest_point' is outside */
    /* or inside the contour corresponding to the edge. */
    /* [note]: This sign may or may not be correct,     */
    /*         therefore it must be checked properly in */
    /*         case there is an ambiguity.              */
    FT_Char       sign;       

  } SDF_Signed_Distance;

  /**************************************************************************
   *
   * constants, initializer and destructor
   *
   */

  static
  const FT_Vector    zero_vector  = { 0, 0 };

  static
  const SDF_Edge     null_edge    = { { 0, 0 }, { 0, 0 },
                                      { 0, 0 }, { 0, 0 },
                                      SDF_EDGE_UNDEFINED };

  static
  const SDF_Contour  null_contour = { { 0, 0 }, { NULL, NULL } };

  static
  const SDF_Shape    null_shape   = { NULL, { NULL, NULL } };

  static
  const SDF_Signed_Distance  max_sdf = { { 0, 0 }, { 0, 0 },
                                         INT_MAX, 0 };

  /* Creates a new `SDF_Edge' on the heap and assigns the `edge' */
  /* pointer to the newly allocated memory.                      */
  static FT_Error
  sdf_edge_new( FT_Memory   memory,
                SDF_Edge**  edge )
  {
    FT_Error   error = FT_Err_Ok;
    SDF_Edge*  ptr   = NULL;


    if ( !memory || !edge )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    FT_QNEW( ptr );
    if ( error == FT_Err_Ok )
    {
      *ptr = null_edge;
      *edge = ptr;
    }

  Exit:
    return error;
  }

  /* Frees the allocated `edge' variable. */
  static void
  sdf_edge_done( FT_Memory   memory,
                 SDF_Edge**  edge )
  {
    if ( !memory || !edge || !*edge )
      return;

    FT_FREE( *edge );
  }

  /* Used in `FT_List_Finalize'. */
  static void
  sdf_edge_destructor( FT_Memory  memory,
                       void*      data,
                       void*      user )
  {
    SDF_Edge*  edge = (SDF_Edge*)data;


    sdf_edge_done( memory, &edge );
  }

  /* Creates a new `SDF_Contour' on the heap and assigns  */
  /* the `contour' pointer to the newly allocated memory. */
  static FT_Error
  sdf_contour_new( FT_Memory      memory,
                   SDF_Contour**  contour )
  {
    FT_Error      error = FT_Err_Ok;
    SDF_Contour*  ptr   = NULL;


    if ( !memory || !contour )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    FT_QNEW( ptr );
    if ( error == FT_Err_Ok )
    {
      *ptr = null_contour;
      *contour = ptr;
    }

  Exit:
    return error;
  }

  /* Frees the allocated `contour' variable and also frees */
  /* the list of edges.                                    */
  static void
  sdf_contour_done( FT_Memory      memory,
                    SDF_Contour**  contour )
  {
    if ( !memory || !contour || !*contour )
      return;

    /*  */
    FT_List_Finalize( &(*contour)->edges, sdf_edge_destructor,
                      memory, NULL );

    FT_FREE( *contour );
  }

  /* Used in `FT_List_Finalize'. */
  static void
  sdf_contour_destructor( FT_Memory  memory,
                          void*      data,
                          void*      user )
  {
    SDF_Contour*  contour = (SDF_Contour*)data;


    sdf_contour_done( memory, &contour );
  }

  /* Creates a new `SDF_Shape' on the heap and assigns  */
  /* the `shape' pointer to the newly allocated memory. */
  static FT_Error
  sdf_shape_new( FT_Memory    memory,
                 SDF_Shape**  shape )
  {
    FT_Error      error = FT_Err_Ok;
    SDF_Shape*    ptr   = NULL;


    if ( !memory || !shape )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    FT_QNEW( ptr );
    if ( error == FT_Err_Ok )
    {
      *ptr = null_shape;
      ptr->memory = memory;
      *shape = ptr;
    }

  Exit:
    return error;
  }

  /* Frees the allocated `shape' variable and also frees */
  /* the list of contours.                               */
  static void
  sdf_shape_done( FT_Memory    memory,
                  SDF_Shape**  shape )
  {
    if ( !memory || !shape || !*shape )
      return;

    /* release the list of contours */
    FT_List_Finalize( &(*shape)->contours, sdf_contour_destructor, 
                       memory, NULL );

    /* release the allocated shape struct  */
    FT_FREE( *shape );
  }
    

  /**************************************************************************
   *
   * shape decomposition functions
   *
   */

  /* This function is called when walking along a new contour */
  /* so add a new contour to the shape's list.                */
  static FT_Error
  sdf_move_to( const FT_26D6_Vec* to,
               void*              user )
  {
    SDF_Shape*    shape    = ( SDF_Shape* )user;
    SDF_Contour*  contour  = NULL;
    FT_ListNode   node     = NULL;

    FT_Error      error    = FT_Err_Ok;
    FT_Memory     memory   = shape->memory;


    if ( !to || !user )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    error = sdf_contour_new( memory, &contour );
    if ( error != FT_Err_Ok )
      goto Exit;

    FT_QNEW( node );
    if ( error != FT_Err_Ok )
      goto Exit;

    contour->last_pos = *to;

    node->data = contour;
    FT_List_Add( &shape->contours, node );

  Exit:
    return error;
  }

  static FT_Error
  sdf_line_to( const FT_26D6_Vec*  to,
               void*               user )
  {
    SDF_Shape*    shape    = ( SDF_Shape* )user;
    SDF_Edge*     edge     = NULL;
    SDF_Contour*  contour  = NULL;
    FT_ListNode   node     = NULL;

    FT_Error      error    = FT_Err_Ok;
    FT_Memory     memory   = shape->memory;


    if ( !to || !user )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    contour = ( SDF_Contour* )shape->contours.tail->data;

    if ( contour->last_pos.x == to->x && 
         contour->last_pos.y == to->y )
      goto Exit;

    error = sdf_edge_new( memory, &edge );
    if ( error != FT_Err_Ok )
      goto Exit;

    FT_QNEW( node );
    if ( error != FT_Err_Ok )
      goto Exit;

    edge->edge_type = SDF_EDGE_LINE;
    edge->start_pos = contour->last_pos;
    edge->end_pos   = *to;

    contour->last_pos = *to;

    node->data = edge;
    FT_List_Add( &contour->edges, node );

  Exit:
    return error;
  }

  static FT_Error
  sdf_conic_to( const FT_26D6_Vec*  control_1,
                const FT_26D6_Vec*  to,
                void*               user )
  {
    SDF_Shape*    shape    = ( SDF_Shape* )user;
    SDF_Edge*     edge     = NULL;
    SDF_Contour*  contour  = NULL;
    FT_ListNode   node     = NULL;

    FT_Error      error    = FT_Err_Ok;
    FT_Memory     memory   = shape->memory;


    if ( !control_1 || !to || !user )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    contour = ( SDF_Contour* )shape->contours.tail->data;

    error = sdf_edge_new( memory, &edge );
    if ( error != FT_Err_Ok )
      goto Exit;

    FT_QNEW( node );
    if ( error != FT_Err_Ok )
      goto Exit;

    edge->edge_type = SDF_EDGE_CONIC;
    edge->start_pos = contour->last_pos;
    edge->control_a = *control_1;
    edge->end_pos   = *to;

    contour->last_pos = *to;

    node->data = edge;
    FT_List_Add( &contour->edges, node );

  Exit:
    return error;
  }

  static FT_Error
  sdf_cubic_to( const FT_26D6_Vec*  control_1,
                const FT_26D6_Vec*  control_2,
                const FT_26D6_Vec*  to,
                void*               user )
  {
    SDF_Shape*    shape    = ( SDF_Shape* )user;
    SDF_Edge*     edge     = NULL;
    SDF_Contour*  contour  = NULL;
    FT_ListNode   node     = NULL;

    FT_Error      error    = FT_Err_Ok;
    FT_Memory     memory   = shape->memory;


    if ( !control_2 || !control_1 || !to || !user )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    contour = ( SDF_Contour* )shape->contours.tail->data;

    error = sdf_edge_new( memory, &edge );
    if ( error != FT_Err_Ok )
      goto Exit;

    FT_QNEW( node );
    if ( error != FT_Err_Ok )
      goto Exit;

    edge->edge_type = SDF_EDGE_CUBIC;
    edge->start_pos = contour->last_pos;
    edge->control_a = *control_1;
    edge->control_b = *control_2;
    edge->end_pos   = *to;

    contour->last_pos = *to;

    node->data = edge;
    FT_List_Add( &contour->edges, node );

  Exit:
    return error;
  }

  FT_DEFINE_OUTLINE_FUNCS(
    sdf_decompose_funcs,

    (FT_Outline_MoveTo_Func)  sdf_move_to,   /* move_to  */
    (FT_Outline_LineTo_Func)  sdf_line_to,   /* line_to  */
    (FT_Outline_ConicTo_Func) sdf_conic_to,  /* conic_to */
    (FT_Outline_CubicTo_Func) sdf_cubic_to,  /* cubic_to */

    0,                                       /* shift    */
    0                                        /* delta    */
  )

  /* function decomposes the outline and puts it into the `shape' struct */
  static FT_Error
  sdf_outline_decompose( FT_Outline*  outline,
                         SDF_Shape*   shape )
  {
    FT_Error  error = FT_Err_Ok;


    if ( !outline || !shape )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    error = FT_Outline_Decompose( outline, &sdf_decompose_funcs, (void*)shape );

  Exit:
    return error;
  }

  /**************************************************************************
   *
   * for debugging
   *
   */

#ifdef FT_DEBUG_LEVEL_TRACE

  static void
  sdf_shape_dump( SDF_Shape*  shape )
  {
    FT_UInt     num_contours = 0;
    FT_UInt     total_edges  = 0;
    FT_ListRec  contour_list;


    if ( !shape )
    {
      FT_TRACE5(( "[sdf] sdf_shape_dump: null shape\n" ));
      return;
    }

    contour_list = shape->contours;

    FT_TRACE5(( "-------------------------------------------------\n" ));
    FT_TRACE5(( "[sdf] sdf_shape_dump:\n" ));

    while ( contour_list.head != NULL )
    {
      FT_UInt       num_edges = 0;
      FT_ListRec    edge_list;
      SDF_Contour*  contour = (SDF_Contour*)contour_list.head->data;


      edge_list = contour->edges;
      FT_TRACE5(( "Contour %d\n", num_contours ));

      while ( edge_list.head != NULL )
      {
        SDF_Edge*  edge = (SDF_Edge*)edge_list.head->data;


        FT_TRACE5(( "    Edge %d\n", num_edges ));

        switch (edge->edge_type) {
        case SDF_EDGE_LINE:
          FT_TRACE5(( "        Edge Type: Line\n" ));
          FT_TRACE5(( "        ---------------\n" ));
          FT_TRACE5(( "        Start Pos: %d, %d\n", edge->start_pos.x,
                                                     edge->start_pos.y ));
          FT_TRACE5(( "        End Pos  : %d, %d\n", edge->end_pos.x,
                                                     edge->end_pos.y ));
          break;
        case SDF_EDGE_CONIC:
          FT_TRACE5(( "        Edge Type: Conic Bezier\n" ));
          FT_TRACE5(( "        -----------------------\n" ));
          FT_TRACE5(( "        Start Pos: %d, %d\n", edge->start_pos.x,
                                                     edge->start_pos.y ));
          FT_TRACE5(( "        Ctrl1 Pos: %d, %d\n", edge->control_a.x,
                                                     edge->control_a.y ));
          FT_TRACE5(( "        End Pos  : %d, %d\n", edge->end_pos.x,
                                                     edge->end_pos.y ));
          break;
        case SDF_EDGE_CUBIC:
          FT_TRACE5(( "        Edge Type: Cubic Bezier\n" ));
          FT_TRACE5(( "        -----------------------\n" ));
          FT_TRACE5(( "        Start Pos: %d, %d\n", edge->start_pos.x,
                                                     edge->start_pos.y ));
          FT_TRACE5(( "        Ctrl1 Pos: %d, %d\n", edge->control_a.x,
                                                     edge->control_a.y ));
          FT_TRACE5(( "        Ctrl2 Pos: %d, %d\n", edge->control_b.x,
                                                     edge->control_b.y ));
          FT_TRACE5(( "        End Pos  : %d, %d\n", edge->end_pos.x,
                                                     edge->end_pos.y ));
          break;
        default:
            break;
        }

        num_edges++;
        total_edges++;
        edge_list.head = edge_list.head->next;
      }

      num_contours++;
      contour_list.head = contour_list.head->next;
    }

    FT_TRACE5(( "\n" ));
    FT_TRACE5(( "*note: the above values are in 26.6 fixed point format*\n" ));
    FT_TRACE5(( "total number of contours = %d\n", num_contours ));
    FT_TRACE5(( "total number of edges    = %d\n", total_edges ));
    FT_TRACE5(( "[sdf] sdf_shape_dump complete\n" ));
    FT_TRACE5(( "-------------------------------------------------\n" ));
  }

#endif

  /**************************************************************************
   *
   * math functions
   *
   */

  /* Original Algorithm: https://github.com/chmike/fpsqrt */
  static FT_Fixed
  square_root( FT_Fixed  val )
  {
    FT_ULong t, q, b, r;


    r = val;
    b = 0x40000000;
    q = 0;
    while( b > 0x40 )
    {
      t = q + b;
      if( r >= t )
      {
        r -= t;
        q = t + b;
      }
      r <<= 1;
      b >>= 1;
    }
    q >>= 8;

    return q;
  }

  /*************************************************************************/
  /*************************************************************************/
  /**                                                                     **/
  /**  RASTERIZER                                                         **/
  /**                                                                     **/
  /*************************************************************************/
  /*************************************************************************/

  /**************************************************************************
   *
   * @Function:
   *   get_min_distance_line
   *
   * @Description:
   *   This function find the shortest distance from the `line' to
   *   a given `point' and assigns it to `out'. Only use it for line
   *   segments.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  get_min_distance_line( SDF_Edge*             line,
                         FT_26D6_Vec           point, 
                         SDF_Signed_Distance*  out )
  {
    /* in order to calculate the shortest distance from a point to */
    /* a line segment.                                             */
    /*                                                             */
    /* a = start point of the line segment                         */
    /* b = end point of the line segment                           */
    /* p = point from which shortest distance is to be calculated  */
    /* ----------------------------------------------------------- */
    /* => we first write the parametric equation of the line       */
    /*    point_on_line = a + ( b - a ) * t ( t is the factor )    */
    /*                                                             */
    /* => next we find the projection of point p on the line. the  */
    /*    projection will be perpendicular to the line, that is    */
    /*    why we can find it by making the dot product zero.       */
    /*    ( point_on_line - a ) . ( p - point_on_line ) = 0        */
    /*                                                             */
    /*                 ( point_on_line )                           */
    /*    ( a ) x-------o----------------x ( b )                   */
    /*                |_|                                          */
    /*                  |                                          */
    /*                  |                                          */
    /*                ( p )                                        */
    /*                                                             */
    /* => by simplifying the above equation we get the factor of   */
    /*    point_on_line such that                                  */
    /*    t = ( ( p - a ) . ( b - a ) ) / ( |b - a| ^ 2 )          */
    /*                                                             */
    /* => we clamp the factor t between [0.0f, 1.0f], because the  */
    /*    point_on_line can be outside the line segment.           */
    /*                                                             */
    /*                                        ( point_on_line )    */
    /*    ( a ) x------------------------x ( b ) -----o---         */
    /*                                              |_|            */
    /*                                                |            */
    /*                                                |            */
    /*                                              ( p )          */
    /*                                                             */
    /* => finally the distance becomes | point_on_line - p |       */

    FT_Error     error = FT_Err_Ok;

    const FT_Vector   a = line->start_pos;
    const FT_Vector   b = line->end_pos;
    const FT_Vector   p = point;

    FT_26D6_Vec  line_segment;      /* `b' - `a'*/
    FT_26D6_Vec  p_sub_a;           /* `p' - `a' */

    FT_26D6      sq_line_length;    /* squared length of `line_segment' */
    FT_16D16     factor;            /* factor of the nearest point      */
    FT_26D6      cross;             /* used to determine sign           */

    FT_16D16_Vec nearest_point;  /* point on the line nearest to `point' */
    FT_16D16_Vec nearest_vector; /* `p' - `nearest_point' */

    if ( !line || !out )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( line->edge_type != SDF_EDGE_LINE )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    line_segment.x = b.x - a.x;
    line_segment.y = b.y - a.y;

    p_sub_a.x = p.x - a.x;
    p_sub_a.y = p.y - a.y;

    sq_line_length = ( line_segment.x * line_segment.x ) / 64 +
                     ( line_segment.y * line_segment.y ) / 64;

    /* currently factor is 26.6 */
    factor = ( p_sub_a.x * line_segment.x ) / 64 +
             ( p_sub_a.y * line_segment.y ) / 64;

    /* now factor is 16.16 */
    factor = FT_DivFix( factor, sq_line_length );

    /* clamp the factor between 0.0 and 1.0 in fixed point */
    if ( factor > FT_INT_16D16( 1 ) )
      factor = FT_INT_16D16( 1 );
    if ( factor < 0 )
      factor = 0;

    nearest_point.x = FT_MulFix( FT_26D6_16D16(line_segment.x),
                                 factor );
    nearest_point.y = FT_MulFix( FT_26D6_16D16(line_segment.y),
                                 factor );

    nearest_point.x = FT_26D6_16D16( a.x ) + nearest_point.x; 
    nearest_point.y = FT_26D6_16D16( a.y ) + nearest_point.y;

    nearest_vector.x = nearest_point.x - FT_26D6_16D16( p.x );
    nearest_vector.y = nearest_point.y - FT_26D6_16D16( p.y );

    cross = FT_MulFix( nearest_vector.x, line_segment.y ) -
            FT_MulFix( nearest_vector.y, line_segment.x );

    FT_Vector_NormLen( &line_segment );

    /* assign the output */
    out->neartest_point = nearest_point;
    out->sign = cross < 0 ? 1 : -1;
    out->squared_distance = FT_MulFix( nearest_vector.x, nearest_vector.x ) +
                            FT_MulFix( nearest_vector.y, nearest_vector.y );
    out->direction = line_segment;

  Exit:
    return error;
  }

  /**************************************************************************
   *
   * @Function:
   *   sdf_edge_get_min_distance
   *
   * @Description:
   *   This function find the shortest distance from the `edge' to
   *   a given `point' and assigns it to `out'.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  sdf_edge_get_min_distance( SDF_Edge*             edge,
                             FT_26D6_Vec           point,
                             SDF_Signed_Distance*  out)
  {
    FT_Error  error = FT_Err_Ok;


    if ( !edge || !out )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    /* edge specific distance calculation */
    switch ( edge->edge_type ) {
    case SDF_EDGE_LINE:
      get_min_distance_line( edge, point, out );
      break;
    case SDF_EDGE_CONIC:
    case SDF_EDGE_CUBIC:
    default:
        error = FT_THROW( Invalid_Argument );
    }

  Exit:
    return error;
  }

  /**************************************************************************
   *
   * @Function:
   *   sdf_contour_get_min_distance
   *
   * @Description:
   *   This function iterate through all the edges that make up
   *   the contour and find the shortest distance from a point to
   *   this contour and assigns it to `out'.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  sdf_contour_get_min_distance( SDF_Contour*          contour,
                                FT_26D6_Vec           point,
                                SDF_Signed_Distance*  out)
  {
    FT_Error             error  = FT_Err_Ok;
    SDF_Signed_Distance  min_dist = max_sdf;
    FT_ListRec           edge_list;


    if ( !contour || !out )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    edge_list = contour->edges;

    /* iterate through all the edges manually */
    while ( edge_list.head ) {
      SDF_Signed_Distance  current_dist = max_sdf;
    
    
      FT_CALL( sdf_edge_get_min_distance( 
               (SDF_Edge*)edge_list.head->data,
               point, &current_dist ) );

      /* [TODO]: *IMPORTANT* Add corner checking function. */
      if ( current_dist.squared_distance >= 0 && 
           current_dist.squared_distance < min_dist.squared_distance )
        min_dist = current_dist;
    
      edge_list.head = edge_list.head->next;
    }

    *out = min_dist;
  Exit:
    return error;
  }

  /**************************************************************************
   *
   * @Function:
   *   sdf_generate
   *
   * @Description:
   *   This is the main function that is responsible for generating
   *   signed distance fields. The function will not align or compute
   *   the size of the `bitmap', therefore setup the `bitmap' properly
   *   and transform the `shape' appropriately before calling this
   *   function.
   *   Currently we check all the pixels against all the contours and
   *   all the edges.
   *
   * @Input:
   *   [TODO]
   *
   * @Return:
   *   [TODO]
   */
  static FT_Error
  sdf_generate( const SDF_Shape*  shape,
                FT_UInt           spread,
                FT_Bitmap*        bitmap )
  {
    FT_Error   error = FT_Err_Ok;
    FT_UInt    width = 0;
    FT_UInt    rows  = 0;
    FT_UInt    x     = 0; /* used to loop in x direction i.e. width */
    FT_UInt    y     = 0; /* used to loop in y direction i.e. rows  */
    FT_UInt    sp_sq = 0; /* `spread' * `spread' int 16.16 fixed    */

    FT_Short*  buffer;

    if ( !shape || !bitmap )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    if ( spread < MIN_SPREAD || spread > MAX_SPREAD )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    width = bitmap->width;
    rows = bitmap->rows;
    buffer = (FT_Short*)bitmap->buffer;

    sp_sq = FT_INT_16D16( spread * spread );

    if ( width == 0 || rows == 0 )
    {
      FT_TRACE0(( "[sdf] sdf_generate:\n"
                  "      Cannot render glyph with width/height == 0\n"
                  "      (width, height provided [%d, %d])", width, rows ));
      error = FT_THROW( Cannot_Render_Glyph );
      goto Exit;
    }

    /* loop through all the rows */
    for ( y = 0; y < rows; y++ )
    {
      /* loop through all the pixels of a row */
      for ( x = 0; x < width; x++ )
      {
        /* `grid_point' is the current pixel position */
        /* our task is to find the shortest distance  */
        /* from this point to the entire shape.       */
        FT_26D6_Vec  grid_point = { FT_INT_26D6( x ),
                                    FT_INT_26D6( y ) };
        SDF_Signed_Distance  min_dist = max_sdf;
        FT_ListRec           contour_list;
        FT_UInt              index;
        FT_Short             value;

        /* This `grid_point' is at the corner, but we */
        /* use the center of the pixel.               */
        grid_point.x += FT_INT_26D6( 1 ) / 2;
        grid_point.y += FT_INT_26D6( 1 ) / 2;

        contour_list = shape->contours;

        index = ( rows - y - 1 ) * width + x;

        /* iterate through all the contours manually */
        while ( contour_list.head ) {
          SDF_Signed_Distance  current_dist = max_sdf;


          FT_CALL( sdf_contour_get_min_distance( 
                   (SDF_Contour*)contour_list.head->data,
                   grid_point, &current_dist ) );

          if ( current_dist.squared_distance < min_dist.squared_distance )
            min_dist = current_dist;

          contour_list.head = contour_list.head->next;
        }

        /* [OPTIMIZATION]: if (min_dist > sp_sq) then simply clamp  */
        /*                 the value to spread to avoid square_root */

        /* clamp the values to spread */
        if ( min_dist.squared_distance > sp_sq )
          min_dist.squared_distance = sp_sq;

        /* square_root the values and fit in a 6.10 fixed point */
        min_dist.squared_distance = square_root( min_dist.squared_distance );

        min_dist.squared_distance /= 64; /* convert from 16.16 to 22.10 */
        value = min_dist.squared_distance & 0x0000FFFF; /* truncate to 6.10 */
        value *= min_dist.sign;

        buffer[index] = value;
      }
    }

  Exit:
    return error;
  }

  /**************************************************************************
   *
   * interface functions
   *
   */

  static FT_Error
  sdf_raster_new( FT_Memory   memory,
                  FT_Raster*  araster)
  {
    FT_Error      error  = FT_Err_Ok;
    SDF_TRaster*  raster = NULL;


    *araster = 0;
    if ( !FT_ALLOC( raster, sizeof( SDF_TRaster ) ) )
    {
      raster->memory = memory;
      *araster = (FT_Raster)raster;
    }

    return error;
  }

  static void
  sdf_raster_reset( FT_Raster       raster,
                    unsigned char*  pool_base,
                    unsigned long   pool_size )
  {
    /* no use of this function */
    FT_UNUSED( raster );
    FT_UNUSED( pool_base );
    FT_UNUSED( pool_size );
  }

  static FT_Error
  sdf_raster_set_mode( FT_Raster      raster,
                       unsigned long  mode,
                       void*          args )
  {
    FT_UNUSED( raster );
    FT_UNUSED( mode );
    FT_UNUSED( args );


    return FT_Err_Ok;
  }

  static FT_Error
  sdf_raster_render( FT_Raster                raster,
                     const FT_Raster_Params*  params )
  {
    FT_Error                  error      = FT_Err_Ok;
    SDF_TRaster*              sdf_raster = (SDF_TRaster*)raster;
    FT_Outline*               outline    = NULL;
    const SDF_Raster_Params*  sdf_params = (const SDF_Raster_Params*)params;

    FT_Memory                 memory     = NULL;
    SDF_Shape*                shape      = NULL;


    /* check for valid arguments */
    if ( !sdf_raster || !sdf_params )
    {
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    outline = (FT_Outline*)sdf_params->root.source;

    /* check if the outline is valid or not */
    if ( !outline )
    {
      error = FT_THROW( Invalid_Outline );
      goto Exit;
    }

    /* if the outline is empty, return */
    if ( outline->n_points <= 0 || outline->n_contours <= 0 )
      goto Exit;

    /* check if the outline has valid fields */
    if ( !outline->contours || !outline->points )
    {
      error = FT_THROW( Invalid_Outline );
      goto Exit;
    }

    /* check if spread is set properly */
    if ( sdf_params->spread > MAX_SPREAD ||
         sdf_params->spread < MIN_SPREAD )
    {
      FT_TRACE0(( 
        "[sdf] sdf_raster_render:\n"
        "      The `spread' field of `SDF_Raster_Params' is invalid,\n"
        "      the value of this field must be within [%d, %d].\n"
        "      Also, you must pass `SDF_Raster_Params' instead of the\n"
        "      default `FT_Raster_Params' while calling this function\n"
        "      and set the fields properly.\n"
        , MIN_SPREAD, MAX_SPREAD) );
      error = FT_THROW( Invalid_Argument );
      goto Exit;
    }

    memory = sdf_raster->memory;
    if ( !memory )
    {
      FT_TRACE0(( "[sdf] sdf_raster_render:\n"
                  "      Raster not setup properly, "
                  "unable to find memory handle.\n" ));
      error = FT_THROW( Invalid_Handle );
      goto Exit;
    }

    FT_CALL( sdf_shape_new( memory, &shape ) );

    FT_CALL( sdf_outline_decompose( outline, shape ) );

    FT_CALL( sdf_generate( shape, sdf_params->spread, 
                           sdf_params->root.target ) );

  Exit:
    if ( shape )
      sdf_shape_done( memory, &shape );

    return error;
  }

  static void
  sdf_raster_done( FT_Raster  raster )
  {
    FT_Memory  memory = (FT_Memory)((SDF_TRaster*)raster)->memory;


    FT_FREE( raster );
  }

  FT_DEFINE_RASTER_FUNCS(
    ft_sdf_raster,

    FT_GLYPH_FORMAT_OUTLINE,

    (FT_Raster_New_Func)      sdf_raster_new,       /* raster_new      */
    (FT_Raster_Reset_Func)    sdf_raster_reset,     /* raster_reset    */
    (FT_Raster_Set_Mode_Func) sdf_raster_set_mode,  /* raster_set_mode */
    (FT_Raster_Render_Func)   sdf_raster_render,    /* raster_render   */
    (FT_Raster_Done_Func)     sdf_raster_done       /* raster_done     */
  )

/* END */
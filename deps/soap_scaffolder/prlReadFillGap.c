/*
 * prlReadFillGap.c
 *
 * Copyright (c) 2008-2012 BGI-Shenzhen <soap at genomics dot org dot cn>.
 *
 * This file is part of SOAPdenovo.
 *
 * SOAPdenovo is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * SOAPdenovo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with SOAPdenovo.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdinc.h"
#include "newhash.h"
#include "kmerhash.h"
#include "extfunc.h"
#include "extvab.h"
#include "zlib.h"

#define RDBLOCKSIZE 50
#define CTGappend 50

static Kmer MAXKMER;
static int Ncounter;
static int allGaps;

// for multi threads
static int * counters;
static pthread_mutex_t mutex;
static int scafBufSize = 100;
static boolean * flagBuf;
static unsigned char * thrdNoBuf;//TODO: remove this as there is no thread structure anymore
static STACK ** ctgStackBuffer;
static int scafCounter;
static int scafInBuf;
static char * scaffBuffer;

static void MarkCtgOccu ( unsigned int ctg );

static void attach1read2contig ( unsigned int ctgID, int len, int pos, long long starter )
{
	unsigned int ctg = index_array[ctgID];  //new index in contig array

	if ( isLargerThanTwin ( ctg ) )
	{
		ctg = getTwinCtg ( ctg ); // put all reads in one contig of a twin
		pos = contig_array[ctg].length + overlaplen - pos - len;
	}

	if ( !contig_array[ctg].closeReads )
	{
		contig_array[ctg].closeReads = ( STACK * ) createStack ( RDBLOCKSIZE, sizeof ( READNEARBY ) );
	}

	READNEARBY * rd = ( READNEARBY * ) stackPush ( contig_array[ctg].closeReads );
	rd->len = len;
	rd->dis = pos;
	rd->seqStarter = starter;
}

static void convertIndex ()
{
	int * length_array = ( int * ) ckalloc ( ( num_ctg + 1 ) * sizeof ( int ) );
	unsigned int i;

	for ( i = 1; i <= num_ctg; i++ )
	{
		length_array[i] = 0;
	}

	for ( i = 1; i <= num_ctg; i++ )
	{
		if ( index_array[i] > 0 )
		{
			length_array[index_array[i]] = i;
		}
	}

	for ( i = 1; i <= num_ctg; i++ )
	{
		index_array[i] = length_array[i];
	}           //contig i with new index: index_array[i]

	free ( ( void * ) length_array );
}

static long long getRead1by1_gz ( gzFile * fp, DARRAY * readSeqInGap )
{
	long long readCounter = 0;

	if ( !fp )
	{
		return readCounter;
	}

	int len, ctgID, pos;
	long long starter;
	char * pt;
	char * freadBuf = ( char * ) ckalloc ( ( maxReadLen / 4 + 1 ) * sizeof ( char ) );

	while ( gzread ( fp, &len, sizeof ( int ) ) == 4 )
	{
		if ( gzread ( fp, &ctgID, sizeof ( int ) ) != 4 )
		{
			break;
		}

		if ( gzread ( fp, &pos, sizeof ( int ) ) != 4 )
		{
			break;
		}

		if ( gzread ( fp, freadBuf, sizeof ( char ) * ( unsigned ) ( len / 4 + 1 ) ) != ( unsigned ) ( len / 4 + 1 ) )
		{
			break;
		}

		//put seq to dynamic array
		starter = readSeqInGap->item_c;

		if ( !darrayPut ( readSeqInGap, starter + len / 4 ) ) // make sure there's room for this seq
		{
			break;
		}

		pt = ( char * ) darrayPut ( readSeqInGap, starter );
		bcopy ( freadBuf, pt, len / 4 + 1 );
		attach1read2contig ( ctgID, len, pos, starter );
		readCounter++;
	}

	free ( ( void * ) freadBuf );
	return readCounter;
}

static long long getRead1by1 ( FILE * fp, DARRAY * readSeqInGap )
{
	long long readCounter = 0;

	if ( !fp )
	{
		return readCounter;
	}

	int len, ctgID, pos;
	long long starter;
	char * pt;
	char * freadBuf = ( char * ) ckalloc ( ( maxReadLen / 4 + 1 ) * sizeof ( char ) );

	while ( fread ( &len, sizeof ( int ), 1, fp ) == 1 )
	{
		if ( fread ( &ctgID, sizeof ( int ), 1, fp ) != 1 )
		{
			break;
		}

		if ( fread ( &pos, sizeof ( int ), 1, fp ) != 1 )
		{
			break;
		}

		if ( fread ( freadBuf, sizeof ( char ), len / 4 + 1, fp ) != ( unsigned ) ( len / 4 + 1 ) )
		{
			break;
		}

		//put seq to dynamic array
		starter = readSeqInGap->item_c;

		if ( !darrayPut ( readSeqInGap, starter + len / 4 ) ) // make sure there's room for this seq
		{
			break;
		}

		pt = ( char * ) darrayPut ( readSeqInGap, starter );
		bcopy ( freadBuf, pt, len / 4 + 1 );
		attach1read2contig ( ctgID, len, pos, starter );
		readCounter++;
	}

	free ( ( void * ) freadBuf );
	return readCounter;
}

// Darray *readSeqInGap
static boolean loadReads4gap ( char * graphfile )
{
	FILE * fp1, *fp2;
	gzFile * fp;
	char name[1024];
	long long readCounter;

	sprintf ( name, "%s.readInGap.gz", graphfile );
	fp = gzopen ( name, "rb" );

	sprintf ( name, "%s.longReadInGap", graphfile );
	fp2 = fopen ( name, "rb" );

	if ( !fp && !fp2 )
	{
		printf ( "Can't open %s.readInGap.gz and %s.longReadInGap!\n", graphfile, graphfile );
		return 0;
	}

	if ( !orig2new )
	{
		convertIndex ();
		orig2new = 1;
	}

	readSeqInGap = ( DARRAY * ) createDarray ( 1000000, sizeof ( char ) );

	if ( fp )
	{
		readCounter = getRead1by1_gz ( fp, readSeqInGap );
		printf ( "Loaded %lld reads from %s.readInGap.\n", readCounter, graphfile );
		gzclose ( fp );
	}

	if ( fp2 )
	{
		readCounter = getRead1by1 ( fp2, readSeqInGap );
		printf ( "Loaded %lld reads from %s.LongReadInGap.\n", readCounter, graphfile );
		fclose ( fp2 );
	}

	return 1;
}

static void debugging1 ()
{
	unsigned int i;

	if ( orig2new )
	{
		unsigned int * length_array = ( unsigned int * ) ckalloc ( ( num_ctg + 1 ) * sizeof ( unsigned int ) );

		//use length_array to change info in index_array
		for ( i = 1; i <= num_ctg; i++ )
		{
			length_array[i] = 0;
		}

		for ( i = 1; i <= num_ctg; i++ )
		{
			if ( index_array[i] > 0 )
			{
				length_array[index_array[i]] = i;
			}
		}

		for ( i = 1; i <= num_ctg; i++ )
		{
			index_array[i] = length_array[i];
		}       //contig i with original index: index_array[i]

		orig2new = 0;
	}

	READNEARBY * rd;
	int j;
	char * pt;

	for ( i = 1; i <= num_ctg; i++ )
	{
		if ( !contig_array[i].closeReads )
		{
			continue;
		}

		if ( index_array[i] != 735 )
		{
			continue;
		}

		printf ( "Contig %d, len %d: \n", index_array[i], contig_array[i].length );
		stackBackup ( contig_array[i].closeReads );

		while ( ( rd = ( READNEARBY * ) stackPop ( contig_array[i].closeReads ) ) != NULL )
		{
			printf ( "%d\t%d\t%lld\t", rd->dis, rd->len, rd->seqStarter );
			pt = ( char * ) darrayGet ( readSeqInGap, rd->seqStarter );

			for ( j = 0; j < rd->len; j++ )
			{
				printf ( "%c", int2base ( ( int ) getCharInTightString ( pt, j ) ) );
			}

			printf ( "\n" );
		}

		stackRecover ( contig_array[i].closeReads );
	}
}

static void initiateCtgInScaf ( CTGinSCAF * actg )
{
	actg->cutTail = 0;
	actg->cutHead = overlaplen;
	actg->gapSeqLen = 0;
}

static int procGap ( char * line, STACK * ctgsStack )
{
	char * tp;
	int length, i, seg;
	unsigned int ctg;
	CTGinSCAF * ctgPt;
	tp = strtok ( line, " " );
	tp = strtok ( NULL, " " );  //length
	length = atoi ( tp );
	tp = strtok ( NULL, " " );  //seg
	seg = atoi ( tp );

	if ( !seg )
	{
		return length;
	}

	for ( i = 0; i < seg; i++ )
	{
		tp = strtok ( NULL, " " );
		ctg = atoi ( tp );
		MarkCtgOccu ( ctg );
		ctgPt = ( CTGinSCAF * ) stackPush ( ctgsStack );
		initiateCtgInScaf ( ctgPt );
		ctgPt->ctgID = ctg;
		ctgPt->start = 0;
		ctgPt->end = 0;
		ctgPt->scaftig_start = 0;
		ctgPt->mask = 1;
	}

	return length;
}

static int cmp_reads ( const void * a, const void * b )
{
	READNEARBY * A, *B;
	A = ( READNEARBY * ) a;
	B = ( READNEARBY * ) b;

	if ( A->dis > B->dis )
	{
		return 1;
	}
	else if ( A->dis == B->dis )
	{
		return 0;
	}
	else
	{
		return -1;
	}
}

static void cutRdArray ( READNEARBY * rdArray, int gapStart, int gapEnd, int * count, int arrayLen, READNEARBY * cutArray )
{
	int i;
	int num = 0;

	for ( i = 0; i < arrayLen; i++ )
	{
		if ( rdArray[i].dis > gapEnd )
		{
			break;
		}

		if ( ( rdArray[i].dis + rdArray[i].len ) >= gapStart )
		{
			cutArray[num].dis = rdArray[i].dis;
			cutArray[num].len = rdArray[i].len;
			cutArray[num++].seqStarter = rdArray[i].seqStarter;
		}
	}

	*count = num;
}


void outputTightStr ( FILE * fp, char * tightStr, int start, int length, int outputlen, int revS, int * col )
{
	int i;
	int end;
	int column = *col;

	if ( !revS )
	{
		end = start + outputlen <= length ? start + outputlen : length;

		for ( i = start; i < end; i++ )
		{
			fprintf ( fp, "%c", int2base ( ( int ) getCharInTightString ( tightStr, i ) ) );

			if ( ( ++column ) % 100 == 0 )
			{
				//column = 0;
				fprintf ( fp, "\n" );
			}
		}
	}
	else
	{
		end = length - start - outputlen - 1 >= 0 ? length - start - outputlen : 0;

		for ( i = length - 1 - start; i >= end; i-- )
		{
			fprintf ( fp, "%c", int2compbase ( ( int ) getCharInTightString ( tightStr, i ) ) );

			if ( ( ++column ) % 100 == 0 )
			{
				fprintf ( fp, "\n" );
				//column = 0;
			}
		}
	}

	*col = column;
}

static void outputTightStrLowerCase ( FILE * fp, char * tightStr, int start, int length, int outputlen, int revS, int * col )
{
	int i;
	int end;
	int column = *col;

	if ( !revS )
	{
		end = start + outputlen <= length ? start + outputlen : length;

		for ( i = start; i < end; i++ )
		{
			fprintf ( fp, "%c", "actg"[ ( int ) getCharInTightString ( tightStr, i )] );

			if ( ( ++column ) % 100 == 0 )
			{
				//column = 0;
				fprintf ( fp, "\n" );
			}
		}
	}
	else
	{
		end = length - start - outputlen - 1 >= 0 ? length - start - outputlen : 0;

		for ( i = length - 1 - start; i >= end; i-- )
		{
			fprintf ( fp, "%c", "tgac"[ ( int ) getCharInTightString ( tightStr, i )] );

			if ( ( ++column ) % 100 == 0 )
			{
				fprintf ( fp, "\n" );
				//column = 0;
			}
		}
	}

	*col = column;
}

static void outputNs ( FILE * fp, int gapN, int * col )
{
	int i, column = *col;

	for ( i = 0; i < gapN; i++ )
	{
		fprintf ( fp, "N" );

		if ( ( ++column ) % 100 == 0 )
		{
			//column = 0;
			fprintf ( fp, "\n" );
		}
	}

	*col = column;
}

static void output1gap ( FILE * fo, int scafIndex, CTGinSCAF * prevCtg, CTGinSCAF * actg, DARRAY * gapSeqArray )
{
	unsigned int ctg1, bal_ctg1, length1;
	int start1, outputlen1;
	unsigned int ctg2, bal_ctg2, length2;
	int start2, outputlen2;
	char * pt;
	int column = 0;
	ctg1 = prevCtg->ctgID;
	bal_ctg1 = getTwinCtg ( ctg1 );
	start1 = prevCtg->cutHead;
	length1 = contig_array[ctg1].length + overlaplen;

	if ( length1 - prevCtg->cutTail - start1 > CTGappend )
	{
		outputlen1 = CTGappend;
		start1 = length1 - prevCtg->cutTail - outputlen1;
	}
	else
	{
		outputlen1 = length1 - prevCtg->cutTail - start1;
	}

	ctg2 = actg->ctgID;
	bal_ctg2 = getTwinCtg ( ctg2 );
	start2 = actg->cutHead;
	length2 = contig_array[ctg2].length + overlaplen;

	if ( length2 - actg->cutTail - start2 > CTGappend )
	{
		outputlen2 = CTGappend;
	}
	else
	{
		outputlen2 = length2 - actg->cutTail - start2;
	}

	if ( isLargerThanTwin ( ctg1 ) )
	{
		fprintf ( fo, ">S%d_C%d_L%d_G%d", scafIndex, index_array[bal_ctg1], outputlen1, prevCtg->gapSeqLen );
	}
	else
	{
		fprintf ( fo, ">S%d_C%d_L%d_G%d", scafIndex, index_array[ctg1], outputlen1, prevCtg->gapSeqLen );
	}

	if ( isLargerThanTwin ( ctg2 ) )
	{
		fprintf ( fo, "_C%d_L%d\t", index_array[bal_ctg2], outputlen2 );
	}
	else
	{
		fprintf ( fo, "_C%d_L%d\t", index_array[ctg2], outputlen2 );
	}

	fprintf ( fo, "%d\n", start2 );

	if ( contig_array[ctg1].seq )
	{
		outputTightStr ( fo, contig_array[ctg1].seq, start1, length1, outputlen1, 0, &column );
	}
	else if ( contig_array[bal_ctg1].seq )
	{
		outputTightStr ( fo, contig_array[bal_ctg1].seq, start1, length1, outputlen1, 1, &column );
	}

	pt = ( char * ) darrayPut ( gapSeqArray, prevCtg->gapSeqOffset );
	outputTightStrLowerCase ( fo, pt, 0, prevCtg->gapSeqLen, prevCtg->gapSeqLen, 0, &column );

	if ( contig_array[ctg2].seq )
	{
		outputTightStr ( fo, contig_array[ctg2].seq, start2, length2, outputlen2, 0, &column );
	}
	else if ( contig_array[bal_ctg2].seq )
	{
		outputTightStr ( fo, contig_array[bal_ctg2].seq, start2, length2, outputlen2, 1, &column );
	}

	if ( column % 100 != 0 )
	{
		fprintf ( fo, "\n" );
	}
}

static void outputGapSeq ( FILE * fo, int index, STACK * ctgsStack, DARRAY * gapSeqArray )
{
	CTGinSCAF * actg, *prevCtg = NULL;
	stackRecover ( ctgsStack );
	//  fprintf (fo, ">scaffold%d\n", index);

	while ( ( actg = stackPop ( ctgsStack ) ) != NULL )
	{
		/*  if (prevCtg)
		    {
		        if (actg->scaftig_start)
		        {
		            fprintf (fo, "0\t%d\t%d\n", prevCtg->mask, actg->mask);
		        }
		        else
		        {
		            fprintf (fo, "1\t%d\t%d\n", prevCtg->mask, actg->mask);
		        }
		    }
		*/
		if ( prevCtg && prevCtg->gapSeqLen > 0 )
			{ output1gap ( fo, index, prevCtg, actg, gapSeqArray ); }

		prevCtg = actg;
	}
}

static void outputScafSeq ( FILE * fo, FILE * foc, int index, STACK * ctgsStack, DARRAY * gapSeqArray )
{
	CTGinSCAF * actg, *prevCtg = NULL;
	unsigned int ctg, bal_ctg, ctg_out, length;
	int start, outputlen, gapN;
	char * pt;
	int column = 0;
	long long cvgSum = 0;
	int lenSum = 0;
	int i, t, lu_len = 0, lu_end = 0;
	unsigned int ctg_start_pos = 0;
	char strand;
	unsigned int * pos_start = ( unsigned int * ) ckalloc ( 1000000 * sizeof ( unsigned int ) );
	unsigned int * pos_end = ( unsigned int * ) ckalloc ( 1000000 * sizeof ( unsigned int ) );
	//  char index_contig[num_ctg][20];
	char ** index_contig = ( char ** ) ckalloc ( 1000000 * sizeof ( char * ) );

	for ( i = 0; i < 1000000; i++ )
		{ index_contig[i] = ( char * ) ckalloc ( 20 * sizeof ( char ) ); }

	char * orien_array;
	orien_array = ( char * ) ckalloc ( 1000000 * sizeof ( char ) );
	//  scaffBuffer = (char *) ckalloc (300000000 * sizeof (char));
	stackRecover ( ctgsStack );

	while ( ( actg = stackPop ( ctgsStack ) ) != NULL )
	{
		if ( ! ( contig_array[actg->ctgID].cvg > 0 ) )
		{
			continue;
		}

		lenSum += contig_array[actg->ctgID].length;
		cvgSum += contig_array[actg->ctgID].length * contig_array[actg->ctgID].cvg;
	}

	if ( lenSum > 0 )
	{
		fprintf ( fo, ">scaffold%d %4.1f\n", index, ( double ) cvgSum / lenSum );
	}
	else
	{
		fprintf ( fo, ">scaffold%d 0.0\n", index );
	}

	fprintf ( foc, ">scaffold%d\n", index );
	stackRecover ( ctgsStack );

	while ( ( actg = stackPop ( ctgsStack ) ) != NULL )
	{
		ctg = actg->ctgID;
		bal_ctg = getTwinCtg ( ctg );
		length = contig_array[ctg].length + overlaplen;

		if ( prevCtg && actg->scaftig_start )
		{
			gapN = actg->start - prevCtg->start - contig_array[prevCtg->ctgID].length;
			if (gapN<0) printf("WARNING: re-adjusting gap of %d bp to 1\n",gapN);
			gapN = gapN > 0 ? gapN : 1; //XXX bj -- BUG: gapN<0 (i.e. overlaps between contigs) end up as 1xN and dups.
			outputNs ( fo,  gapN, &column );
			ctg_start_pos += gapN;
			//outputGapInfo(prevCtg->ctgID,ctg);
			Ncounter++;
		}

		if ( !prevCtg )
		{
			start = 0;
		}
		else
		{
			start = actg->cutHead;
		}

		outputlen = length - start - actg->cutTail;

		if ( contig_array[ctg].seq )
		{
			outputTightStr ( fo, contig_array[ctg].seq, start, length, outputlen, 0, &column );
			lu_end = start + outputlen > length ? length : start + outputlen;
			lu_len = lu_end - start;
			strand = '+';
			fprintf ( foc, "%d\t", index_array[ctg] );
		}
		else if ( contig_array[bal_ctg].seq )
		{
			outputTightStr ( fo, contig_array[bal_ctg].seq, start, length, outputlen, 1, &column );
			lu_end = length - start - outputlen - 1 >= 0 ? length - start - outputlen : 0;
			lu_len = length - start - lu_end;
			strand = '-';
			fprintf ( foc, "%d\t", index_array[bal_ctg] );
		}

		fprintf ( foc, "%u\t%c\t%d\n", ctg_start_pos, strand, lu_len );
		ctg_start_pos += lu_len;

		if ( actg->gapSeqLen < 1 )
		{
			prevCtg = actg;
			continue;
		}

		pt = ( char * ) darrayPut ( gapSeqArray, actg->gapSeqOffset );
		outputTightStrLowerCase ( fo, pt, 0, actg->gapSeqLen, actg->gapSeqLen, 0, &column );
		ctg_start_pos = ctg_start_pos + actg->gapSeqLen;
		prevCtg = actg;
	}

	if ( column % 100 != 0 )
	{
		fprintf ( fo, "\n" );
	}

	free ( ( void * ) pos_start );
	free ( ( void * ) pos_end );
	free ( ( void * ) orien_array );

	for ( i = 0; i < 1000000; i++ )
	{
		free ( ( void * ) index_contig[i] );
	}

	free ( ( void * ) index_contig );
}

static void fill1scaf ( int index, STACK * ctgsStack, int thrdID );
static void check1scaf ( int t, int thrdID )
{
	if ( flagBuf[t] )
	{
		return;
	}

	boolean late = 0;
	pthread_mutex_lock ( &mutex );

	if ( !flagBuf[t] )
	{
		flagBuf[t] = 1;
		thrdNoBuf[t] = thrdID;
	}
	else
	{
		late = 1;
	}

	pthread_mutex_unlock ( &mutex );

	if ( late )
	{
		return;
	}

	counters[thrdID]++;
	fill1scaf ( scafCounter + t + 1, ctgStackBuffer[t], thrdID );
}

static void fill1scaf ( int index, STACK * ctgsStack, int thrdID )
{
	CTGinSCAF * actg, *prevCtg = NULL;
	READNEARBY * rdArray, *rdArray4gap, *rd;
	int numRd = 0, count, maxGLen = 0;
	unsigned int ctg, bal_ctg;
	STACK * rdStack;

	while ( ( actg = stackPop ( ctgsStack ) ) != NULL )
	{
		if ( prevCtg )
		{
			maxGLen = maxGLen < ( actg->start - prevCtg->end ) ? ( actg->start - prevCtg->end ) : maxGLen;
		}

		ctg = actg->ctgID;
		bal_ctg = getTwinCtg ( ctg );

		if ( actg->mask )
		{
			prevCtg = actg;
			continue;
		}

		if ( contig_array[ctg].closeReads )
		{
			numRd += contig_array[ctg].closeReads->item_c;
		}
		else if ( contig_array[bal_ctg].closeReads )
		{
			numRd += contig_array[bal_ctg].closeReads->item_c;
		}

		prevCtg = actg;
	}

	if ( numRd < 1 )
	{
		return;
	}

	rdArray = ( READNEARBY * ) ckalloc ( numRd * sizeof ( READNEARBY ) );
	rdArray4gap = ( READNEARBY * ) ckalloc ( numRd * sizeof ( READNEARBY ) );
	//fprintf(stderr,"scaffold%d reads4gap %d\n",index,numRd);
	// collect reads appended to contigs in this scaffold
	int numRd2 = 0;
	stackRecover ( ctgsStack );

	while ( ( actg = stackPop ( ctgsStack ) ) != NULL )
	{
		ctg = actg->ctgID;
		bal_ctg = getTwinCtg ( ctg );

		if ( actg->mask )
		{
			continue;
		}

		if ( contig_array[ctg].closeReads )
		{
			rdStack = contig_array[ctg].closeReads;
		}
		else if ( contig_array[bal_ctg].closeReads )
		{
			rdStack = contig_array[bal_ctg].closeReads;
		}
		else
		{
			continue;
		}

		stackBackup ( rdStack );

		while ( ( rd = ( READNEARBY * ) stackPop ( rdStack ) ) != NULL )
		{
			rdArray[numRd2].len = rd->len;
			rdArray[numRd2].seqStarter = rd->seqStarter;

			if ( isSmallerThanTwin ( ctg ) )
			{
				rdArray[numRd2++].dis = actg->start - overlaplen + rd->dis;
			}
			else
				{ rdArray[numRd2++].dis = actg->start - overlaplen + contig_array[ctg].length - rd->len - rd->dis; }
		}

		stackRecover ( rdStack );
	}

	if ( numRd2 != numRd )
	{
		printf ( "##reads numbers doesn't match, %d vs %d when scaffold %d.\n", numRd, numRd2, index );
	}

	qsort ( rdArray, numRd, sizeof ( READNEARBY ), cmp_reads );
	//fill gap one by one
	int gapStart, gapEnd;
	int numIn = 0;
	boolean flag;
	int buffer_size = maxReadLen > 100 ? maxReadLen : 100;
	int maxGSLen = maxGLen + GLDiff < 10 ? 10 : maxGLen + GLDiff;
	char * seqGap = ( char * ) ckalloc ( maxGSLen * sizeof ( char ) ); // temp array for gap sequence
	Kmer * kmerCtg1 = ( Kmer * ) ckalloc ( buffer_size * sizeof ( Kmer ) );
	Kmer * kmerCtg2 = ( Kmer * ) ckalloc ( buffer_size * sizeof ( Kmer ) );
	char * seqCtg1 = ( char * ) ckalloc ( buffer_size * sizeof ( char ) );
	char * seqCtg2 = ( char * ) ckalloc ( buffer_size * sizeof ( char ) );
	prevCtg = NULL;
	stackRecover ( ctgsStack );

	while ( ( actg = stackPop ( ctgsStack ) ) != NULL )
	{
		if ( !prevCtg || !actg->scaftig_start )
		{
			prevCtg = actg;
			continue;
		}

		gapStart = prevCtg->end - 100;
		gapEnd = actg->start - overlaplen + 100;
		cutRdArray ( rdArray, gapStart, gapEnd, &count, numRd, rdArray4gap );
		numIn += count;
		/*
		   if(!count){
		   prevCtg = actg;
		   continue;
		   }
		 */
		int overlap;

		for ( overlap = overlaplen; overlap > 14; overlap -= 2 )
		{
			flag = localGraph ( rdArray4gap, count, prevCtg, actg, overlaplen, kmerCtg1, kmerCtg2, overlap, darrayBuf[thrdID], seqCtg1, seqCtg2, seqGap );

			//free_kmerset(kmerSet);

			if ( flag == 1 )
			{
				/*
				   fprintf(stderr,"Between ctg %d and %d, Found with %d\n",prevCtg->ctgID
				   ,actg->ctgID,overlap);
				 */
				break;
			}
		}

		prevCtg = actg;
	}

	//fprintf(stderr,"____scaffold%d reads in gap %d\n",index,numIn);
	free ( ( void * ) seqGap );
	free ( ( void * ) kmerCtg1 );
	free ( ( void * ) kmerCtg2 );
	free ( ( void * ) seqCtg1 );
	free ( ( void * ) seqCtg2 );
	free ( ( void * ) rdArray );
	free ( ( void * ) rdArray4gap );
}

static void reverseStack ( STACK * dStack, STACK * sStack )
{
	CTGinSCAF * actg, *ctgPt;
	emptyStack ( dStack );

	while ( ( actg = ( CTGinSCAF * ) stackPop ( sStack ) ) != NULL )
	{
		ctgPt = ( CTGinSCAF * ) stackPush ( dStack );
		ctgPt->ctgID = actg->ctgID;
		ctgPt->start = actg->start;
		ctgPt->end = actg->end;
		ctgPt->scaftig_start = actg->scaftig_start;
		ctgPt->mask = actg->mask;
		ctgPt->cutHead = actg->cutHead;
		ctgPt->cutTail = actg->cutTail;
		ctgPt->gapSeqLen = actg->gapSeqLen;
		ctgPt->gapSeqOffset = actg->gapSeqOffset;
	}

	stackBackup ( dStack );
}

#ifdef MER127
static Kmer tightStr2Kmer ( char * tightStr, int start, int length, int revS )
{
	int i;
	Kmer word;
	word.high1 = word.low1 = word.high2 = word.low2 = 0;

	if ( !revS )
	{
		if ( start + overlaplen > length )
		{
			printf ( "The tightStr2Kmer A: no enough bases for kmer.\n" );
			return word;
		}

		for ( i = start; i < start + overlaplen; i++ )
		{
			word = KmerLeftBitMoveBy2 ( word );
			word.low2 |= getCharInTightString ( tightStr, i );
		}
	}
	else
	{
		if ( length - start - overlaplen < 0 )
		{
			printf ( "The tightStr2Kmer B: no enough bases for kmer.\n" );
			return word;
		}

		for ( i = length - 1 - start; i > length - 1 - start - overlaplen; i-- )
		{
			word = KmerLeftBitMoveBy2 ( word );
			word.low2 |= int_comp ( getCharInTightString ( tightStr, i ) );
		}
	}

	return word;
}

static Kmer maxKmer ()
{
	Kmer word;
	word.high1 = word.low1 = word.high2 = word.low2 = 0;
	int i;

	for ( i = 0; i < overlaplen; i++ )
	{
		word = KmerLeftBitMoveBy2 ( word );
		word.low2 |= 0x3;
	}

	return word;
}
#else
static Kmer tightStr2Kmer ( char * tightStr, int start, int length, int revS )
{
	int i;
	Kmer word;
	word.high = word.low = 0;

	if ( !revS )
	{
		if ( start + overlaplen > length )
		{
			printf ( "The tightStr2Kmer A: no enough bases for kmer.\n" );
			return word;
		}

		for ( i = start; i < start + overlaplen; i++ )
		{
			word = KmerLeftBitMoveBy2 ( word );
			word.low |= getCharInTightString ( tightStr, i );
		}
	}
	else
	{
		if ( length - start - overlaplen < 0 )
		{
			printf ( "The tightStr2Kmer B: no enough bases for kmer.\n" );
			return word;
		}

		for ( i = length - 1 - start; i > length - 1 - start - overlaplen; i-- )
		{
			word = KmerLeftBitMoveBy2 ( word );
			word.low |= int_comp ( getCharInTightString ( tightStr, i ) );
		}
	}

	return word;
}

static Kmer maxKmer ()
{
	Kmer word;
	word.high = word.low = 0;
	int i;

	for ( i = 0; i < overlaplen; i++ )
	{
		word = KmerLeftBitMoveBy2 ( word );
		word.low |= 0x3;
	}

	return word;
}
#endif


static void initStackBuf ( STACK ** ctgStackBuffer, int scafBufSize )
{
	int i;

	for ( i = 0; i < scafBufSize; i++ )
	{
		flagBuf[i] = 1;
		ctgStackBuffer[i] = ( STACK * ) createStack ( 100, sizeof ( CTGinSCAF ) );
	}
}
static void freeStackBuf ( STACK ** ctgStackBuffer, int scafBufSize )
{
	int i;

	for ( i = 0; i < scafBufSize; i++ )
	{
		freeStack ( ctgStackBuffer[i] );
	}
}

static void threadRoutine ( void * para )
{
	PARAMETER * prm;
	int i;
	prm = ( PARAMETER * ) para;

	//printf("%dth thread with threadID %d, hash_table %p\n",id,prm.threadID,prm.hash_table);
	while ( 1 )
	{
		if ( * ( prm->selfSignal ) == 1 )
		{
			emptyDarray ( darrayBuf[prm->threadID] );

			for ( i = 0; i < scafInBuf; i++ )
			{
				check1scaf ( i, prm->threadID );
			}

			* ( prm->selfSignal ) = 0;
		}
		else if ( * ( prm->selfSignal ) == 2 )
		{
			* ( prm->selfSignal ) = 0;
			break;
		}

		usleep ( 1 );
	}
}

static void outputSeqs ( FILE * fo, FILE * foc, FILE * fo2, int scafInBuf )
{
	int i, thrdID;

	for ( i = 0; i < scafInBuf; i++ )
	{
		thrdID = thrdNoBuf[i];
		outputScafSeq ( fo, foc, scafCounter + i + 1, ctgStackBuffer[i], darrayBuf[thrdID] );
		outputGapSeq ( fo2, scafCounter + i + 1, ctgStackBuffer[i], darrayBuf[thrdID] );
	}
}

static void MaskContig ( unsigned int ctg )
{
	contig_array[ctg].mask = 1;
	contig_array[getTwinCtg ( ctg )].mask = 1;
}

static void MarkCtgOccu ( unsigned int ctg )
{
	contig_array[ctg].flag = 1;
	contig_array[getTwinCtg ( ctg )].flag = 1;
}

static void output_ctg ( unsigned int ctg, FILE * fo )
{
	if ( contig_array[ctg].length < 1 )
	{
		return;
	}

	int len;
	unsigned int bal_ctg = getTwinCtg ( ctg );
	len = contig_array[ctg].length + overlaplen;
	int col = 0;

	if ( contig_array[ctg].seq )
	{
		fprintf ( fo, ">C%d %4.1f\n", ctg, ( double ) contig_array[ctg].cvg );
		outputTightStr ( fo, contig_array[ctg].seq, 0, len, len, 0, &col );
	}
	else if ( contig_array[bal_ctg].seq )
	{
		fprintf ( fo, ">C%d %4.1f\n", bal_ctg, ( double ) contig_array[ctg].cvg );
		outputTightStr ( fo, contig_array[bal_ctg].seq, 0, len, len, 0, &col );
	}

	contig_array[ctg].flag = 1;
	contig_array[bal_ctg].flag = 1;

	if ( len % 100 != 0 )
	{
		fprintf ( fo, "\n" );
	}
}

void prlReadsCloseGap ( char * graphfile )
{

	if ( orig2new )
	{
		convertIndex ();
		orig2new = 0;
	}

	FILE * fp, *fo, *fo2, *fo3 = NULL, *foc, *foc2 = NULL;
	char line[1024];
	CTGinSCAF * actg;
	STACK * ctgStack, *aStack;
	int index = 0, offset = 0, counter, overallLen;
	int i, starter, prev_start, gapLen, catchable, scafnum;
	unsigned int ctg, prev_ctg = 0;
	boolean IsPrevGap;
	pthread_t threads[thrd_num];
	unsigned char thrdSignal[thrd_num + 1];
	PARAMETER paras[thrd_num];

	for ( ctg = 1; ctg <= num_ctg; ctg++ )
	{
		contig_array[ctg].flag = 0;
	}

	MAXKMER = maxKmer ();
	ctgStack = ( STACK * ) createStack ( 1000, sizeof ( CTGinSCAF ) );
	sprintf ( line, "%s.scaf_gap", graphfile );
	fp = ckopen ( line, "r" );
	sprintf ( line, "%s.scafSeq", graphfile );
	fo = ckopen ( line, "w" );
	sprintf ( line, "%s.contigPosInscaff", graphfile );
	foc = ckopen ( line, "w" );

	sprintf ( line, "%s.gapSeq", graphfile );
	fo2 = ckopen ( line, "w" );
	pthread_mutex_init ( &mutex, NULL );
	flagBuf = ( boolean * ) ckalloc ( scafBufSize * sizeof ( boolean ) );;
	thrdNoBuf = ( unsigned char * ) ckalloc ( scafBufSize * sizeof ( unsigned char ) );;
	memset ( thrdNoBuf, 0, scafBufSize * sizeof ( char ) );
	ctgStackBuffer = ( STACK ** ) ckalloc ( scafBufSize * sizeof ( STACK * ) );
	initStackBuf ( ctgStackBuffer, scafBufSize );
	darrayBuf = ( DARRAY ** ) ckalloc ( thrd_num * sizeof ( DARRAY * ) );
	counters = ( int * ) ckalloc ( thrd_num * sizeof ( int ) );
	contig_index_array = ( int * ) ckalloc ( ( num_ctg + 1 ) * sizeof ( int ) );

	for ( i = 0; i <= num_ctg; i++ )
	{
		contig_index_array[i] = 0;
	}

	for ( i = 0; i < thrd_num; i++ )
	{
		counters[i] = 0;
		darrayBuf[i] = ( DARRAY * ) createDarray ( 100000, sizeof ( char ) );
		thrdSignal[i + 1] = 0;
		paras[i].threadID = i;
		paras[i].mainSignal = &thrdSignal[0];
		paras[i].selfSignal = &thrdSignal[i + 1];
	}



	Ncounter = scafCounter = scafInBuf = allGaps = 0;

	while ( fgets ( line, sizeof ( line ), fp ) != NULL )
	{
		if ( line[0] == '>' )
		{
			if ( index )
			{
				aStack = ctgStackBuffer[scafInBuf];
				flagBuf[scafInBuf++] = 0;
				reverseStack ( aStack, ctgStack );

				if ( scafInBuf == scafBufSize )
				{
					outputSeqs ( fo, foc, fo2, scafInBuf );
					scafCounter += scafInBuf;
					scafInBuf = 0;
				}

				if ( index % 1000 == 0 )
				{
					printf ( "%d scaffolds processed.\n", index );
				}
			}

			//read next scaff
			emptyStack ( ctgStack );
			IsPrevGap = offset = prev_ctg = 0;
			sscanf ( line + 9, "%d %d %d", &index, &counter, &overallLen );
			continue;
		}

		if ( line[0] == 'G' ) // gap appears
		{
			continue;
		}

		if ( line[0] >= '0' && line[0] <= '9' ) // a contig line
		{
			sscanf ( line, "%d %d", &ctg, &starter );
			actg = ( CTGinSCAF * ) stackPush ( ctgStack );
			actg->ctgID = ctg;

			if ( contig_array[ctg].flag )
			{
				MaskContig ( ctg );
			}
			else
			{
				MarkCtgOccu ( ctg );
			}

			initiateCtgInScaf ( actg );

			if ( !prev_ctg )
			{
				actg->cutHead = 0;
			}
			else if ( !IsPrevGap )
			{
				allGaps++;
			}

			if ( !IsPrevGap )
			{
				if ( prev_ctg && ( starter - prev_start - ( int ) contig_array[prev_ctg].length ) < ( ( int ) overlaplen * 4 ) )
				{
					catchable = 0;

					if ( catchable ) // prev_ctg and ctg overlap **bp
					{
						allGaps--;
						actg->scaftig_start = 0;
						actg->cutHead = catchable;
						offset += - ( starter - prev_start - contig_array[prev_ctg].length ) + ( overlaplen - catchable );
					}
					else
					{
						actg->scaftig_start = 1;
					}
				}
				else
				{
					actg->scaftig_start = 1;
				}
			}
			else
			{
				offset += - ( starter - prev_start - contig_array[prev_ctg].length ) + gapLen;
				actg->scaftig_start = 0;
			}

			actg->start = starter + offset;
			actg->end = actg->start + contig_array[ctg].length - 1;
			actg->mask = contig_array[ctg].mask;
			IsPrevGap = 0;
			prev_ctg = ctg;
			prev_start = starter;
		}
	}

	if ( index )
	{
		aStack = ctgStackBuffer[scafInBuf];
		flagBuf[scafInBuf++] = 0;
		reverseStack ( aStack, ctgStack );


		outputSeqs ( fo, foc, fo2, scafInBuf );
	}

	for ( ctg = 1; ctg <= num_ctg; ctg++ )
	{
		if ( ( contig_array[ctg].length + overlaplen ) < 100 || contig_array[ctg].flag )
		{
			continue;
		}

		output_ctg ( ctg, fo );
	}

	printf ( "\nDone with %d scaffolds, %d gaps finished, %d gaps overall.\n", index, allGaps - Ncounter, allGaps );
	index = 0;

	for ( i = 0; i < thrd_num; i++ )
	{
		freeDarray ( darrayBuf[i] );
		index += counters[i];
	}

	free ( ( void * ) darrayBuf );

	if ( readSeqInGap )
	{
		freeDarray ( readSeqInGap );
	}

	fclose ( fp );
	fclose ( fo );
	fclose ( foc );
	fclose ( fo2 );

	freeStack ( ctgStack );
	freeStackBuf ( ctgStackBuffer, scafBufSize );
	free ( ( void * ) flagBuf );
	free ( ( void * ) thrdNoBuf );
	free ( ( void * ) ctgStackBuffer );
}

/*
 * prlRead2Ctg.c
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

static long long readsInGap = 0;

static int buffer_size = 100000000;
static long long readCounter;
static long long mapCounter;
static int ALIGNLEN = 0;

//buffer related varibles for chop kmer
static int read_c;
static char ** rcSeq;
static char ** seqBuffer;
static int * lenBuffer;
static unsigned int * ctgIdArray;
static int * posArray;
static char * orienArray;
static char * footprint;    // flag indicates whether the read shoulld leave markers on contigs
static char ** read_name;

// kmer related variables
static int kmer_c;
static Kmer * kmerBuffer;
static ubyte8 * hashBanBuffer;
static kmer_t ** nodeBuffer;
static boolean * smallerBuffer;
static unsigned int * indexArray;
static int * insSizeArray;

static int * deletion;
static void parse1read ( int t );
static void threadRoutine ( void * thrdID );
static void searchKmer ( int t, KmerSet * kset );
static void chopKmer4read ( int t, int threadID );
static void thread_wait ( pthread_t * threads );

static void creatThrds ( pthread_t * threads, PARAMETER * paras )
{
	unsigned char i;
	int temp;

	for ( i = 0; i < thrd_num; i++ )
	{
		if ( ( temp = pthread_create ( &threads[i], NULL, ( void * ) threadRoutine, & ( paras[i] ) ) ) != 0 )
		{
			printf ( "Create threads failed.\n" );
			exit ( 1 );
		}
	}

	printf ( "%d thread(s) initialized.\n", thrd_num );
}

static void threadRoutine ( void * para )
{
	PARAMETER * prm;
	int i, t;
	unsigned char id;
	prm = ( PARAMETER * ) para;
	id = prm->threadID;

	while ( 1 )
	{
		if ( * ( prm->selfSignal ) == 1 )
		{
			for ( i = 0; i < kmer_c; i++ )
			{
				if ( ( hashBanBuffer[i] % thrd_num ) != id )
				{
					continue;
				}

				searchKmer ( i, KmerSets[id] );
			}

			* ( prm->selfSignal ) = 0;
		}
		else if ( * ( prm->selfSignal ) == 2 )
		{
			for ( i = 0; i < read_c; i++ )
			{
				if ( i % thrd_num != id )
				{
					continue;
				}

				chopKmer4read ( i, id + 1 );
			}

			* ( prm->selfSignal ) = 0;
		}
		else if ( * ( prm->selfSignal ) == 3 )
		{
			// parse reads
			for ( t = 0; t < read_c; t++ )
			{
				if ( t % thrd_num != id )
				{
					continue;
				}

				parse1read ( t );
			}

			* ( prm->selfSignal ) = 0;
		}
		else if ( * ( prm->selfSignal ) == 5 )
		{
			* ( prm->selfSignal ) = 0;
			break;
		}

		usleep ( 1 );
	}
}

static void chopKmer4read ( int t, int threadID )
{
	int len_seq = lenBuffer[t];

	if ( len_seq < overlaplen + 1 )
	{
		return;
	}

	char * src_seq = seqBuffer[t];
	char * bal_seq = rcSeq[threadID];
	int j, bal_j;
	ubyte8 hash_ban, bal_hash_ban;
	Kmer word, bal_word;
	int index;
#ifdef MER127
	word.high1 = word.low1 = word.high2 = word.low2 = 0;

	for ( index = 0; index < overlaplen; index++ )
	{
		word = KmerLeftBitMoveBy2 ( word );
		word.low2 |= src_seq[index];
	}

#else
	word.high = word.low = 0;

	for ( index = 0; index < overlaplen; index++ )
	{
		word = KmerLeftBitMoveBy2 ( word );
		word.low |= src_seq[index];
	}

#endif
	reverseComplementSeq ( src_seq, len_seq, bal_seq );
	// complementary node
	bal_word = reverseComplement ( word, overlaplen );
	bal_j = len_seq - 0 - overlaplen;
	index = indexArray[t];

	if ( KmerSmaller ( word, bal_word ) )
	{
		hash_ban = hash_kmer ( word );
		kmerBuffer[index] = word;
		smallerBuffer[index] = 1;
		hashBanBuffer[index++] = hash_ban;
	}
	else
	{
		bal_hash_ban = hash_kmer ( bal_word );
		kmerBuffer[index] = bal_word;
		smallerBuffer[index] = 0;
		hashBanBuffer[index++] = bal_hash_ban;
	}

	for ( j = 1; j <= len_seq - overlaplen; j++ )
	{
		word = nextKmer ( word, src_seq[j - 1 + overlaplen] );
		bal_j = len_seq - j - overlaplen;
		bal_word = prevKmer ( bal_word, bal_seq[bal_j] );

		if ( KmerSmaller ( word, bal_word ) )
		{
			hash_ban = hash_kmer ( word );
			kmerBuffer[index] = word;
			smallerBuffer[index] = 1;
			hashBanBuffer[index++] = hash_ban;
		}
		else
		{
			// complementary node
			bal_hash_ban = hash_kmer ( bal_word );
			kmerBuffer[index] = bal_word;
			smallerBuffer[index] = 0;
			hashBanBuffer[index++] = bal_hash_ban;
		}
	}
}

//splay for one kmer in buffer and save the node to nodeBuffer
static void searchKmer ( int t, KmerSet * kset )
{
	kmer_t * node;
	boolean found = search_kmerset ( kset, kmerBuffer[t], &node );

	if ( found && !node->deleted )
	{
		nodeBuffer[t] = node;
	}
	else
	{
		nodeBuffer[t] = NULL;
	}
}

/*************************************************
Function:
    parse1read
Description:
    Aligns read to contig by choosing the contig on which the read has most kmers hits.
Input:
    1. t:           read index
Output:
    None.
Return:
    None.
*************************************************/
static void parse1read ( int t )
{
	unsigned int j, i, s;
	unsigned int contigID;
	int counter = 0, counter2 = 0;
	unsigned int ctgLen, pos = 0;
	kmer_t * node;
	boolean isSmaller;
	int flag, maxOcc = 0;
	int alldgnLen = lenBuffer[t] > ALIGNLEN ? ALIGNLEN : lenBuffer[t];
	int multi = alldgnLen - overlaplen + 1 < 2 ? 2 : alldgnLen - overlaplen + 1; //how many kmers for placement?
	unsigned int start, finish;
	footprint[t] = 0;
	start = indexArray[t];
	finish = indexArray[t + 1];

	if ( finish == start )
	{
		ctgIdArray[t] = 0;
		return;
	}
	//from begin to end of the kmers in the read
	for ( j = start; j < finish; j++ )
	{
		node = nodeBuffer[j];

		if ( !node ) //unplaced or already processed
		{
			continue;
		}

		flag = 1; //alignement starts with 1 kmer

		for ( s = j + 1; s < finish; s++ )
		{
			if ( !nodeBuffer[s] )//unplaced or already used
			{
				continue;
			}

			if ( nodeBuffer[s]->l_links == node->l_links ) //placed and matches, increase count and delete placement
			{
				flag++;
				nodeBuffer[s] = NULL;
			}
		}

		if ( ( overlaplen < 32 && flag >= 2 ) || overlaplen > 32 ) //not very sure what counter2 is
		{
			counter2++;
		}

		if ( flag >= multi ) //if the match is long enough to be valid
		{
			counter++; //increase the valid matches count
			if ( flag > maxOcc ) //is this the largest match yet?
			{
				pos = j;
				maxOcc = flag;
			}
		}

	}

	//XXX: bj mod was --> if ( !counter )
	if ( counter!=1 )
	{
		ctgIdArray[t] = 0;
		return;
	}

	if ( counter2 > 1 )
	{
		footprint[t] = 1;
	}
	//pos at this point has the END of the match
	j = pos;
	i = pos - start + 1;
	node = nodeBuffer[j];
	isSmaller = smallerBuffer[j];
	contigID = node->l_links;
	ctgLen = contig_array[contigID].length;
	pos = node->r_links;

	if ( node->twin == isSmaller )
	{
		orienArray[t] = '-';
		ctgIdArray[t] = getTwinCtg ( contigID );
		posArray[t] = ctgLen - pos - overlaplen - i + 1;
	}
	else
	{
		orienArray[t] = '+';
		ctgIdArray[t] = contigID;
		posArray[t] = pos - i + 1;
	}
}

static void sendWorkSignal ( unsigned char SIG, unsigned char * thrdSignals )
{
	int t;

	for ( t = 0; t < thrd_num; t++ )
	{
		thrdSignals[t + 1] = SIG;
	}

	while ( 1 )
	{
		usleep ( 10 );

		for ( t = 0; t < thrd_num; t++ )
			if ( thrdSignals[t + 1] )
			{
				break;
			}

		if ( t == thrd_num )
		{
			break;
		}
	}
}

static void locate1read ( int t )
{
	int i, j, start, finish;
	kmer_t * node;
	unsigned int contigID;
	int pos, ctgLen;
	boolean isSmaller;
	start = indexArray[t];
	finish = indexArray[t + 1];

	for ( j = start; j < finish; j++ )
	{
		node = nodeBuffer[j];

		if ( !node ) //same as previous
		{
			continue;
		}

		i = j - start + 1;
		isSmaller = smallerBuffer[j];
		contigID = node->l_links;
		ctgLen = contig_array[contigID].length;
		pos = node->r_links;

		if ( node->twin == isSmaller )
		{
			ctgIdArray[t] = getTwinCtg ( contigID );
			posArray[t] = ctgLen - pos - overlaplen - i + 1;
		}
		else
		{
			ctgIdArray[t] = contigID;
			posArray[t] = pos - i + 1;
		}
	}
}

static void output1read_gz ( int t, gzFile * outfp)
{
	int len = lenBuffer[t];
	int index;
	readsInGap++;

	for ( index = 0; index < len; index++ )
	{
		writeChar2tightString ( seqBuffer[t][index], rcSeq[1], index );
	}

	gzwrite ( outfp, &len, sizeof ( int ) );
	gzwrite ( outfp, &ctgIdArray[t], sizeof ( int ) );
	gzwrite ( outfp, &posArray[t], sizeof ( int ) );
	gzwrite ( outfp, rcSeq[1], ( unsigned ) ( len / 4 + 1 ) );
}



static void getReadIngap ( int t, int insSize, gzFile * outfp1, boolean readOne )
{
	int read1, read2;

	if ( readOne )
	{
		read1 = t;
		read2 = t + 1;

		ctgIdArray[read1] = ctgIdArray[read2];
		posArray[read1] = posArray[read2] + insSize - lenBuffer[read1];
		output1read_gz ( read1, outfp1);
	}
	else
	{
		read2 = t;
		read1 = t - 1;

		ctgIdArray[read2] = ctgIdArray[read1];
		posArray[read2] = posArray[read1] + insSize - lenBuffer[read2]; // --> R1     <-- R2
		output1read_gz ( read2, outfp1);
	}
}

/**
 * This outputs the reads as mapped into the different files
 * outfp is the important file (i.e. readsOnContig.gz)
 **/

static void recordAlldgn ( gzFile * outfp, int * insSizeArr, gzFile * outfp1 )
{
	int t, ctgId;
	boolean rd1gap, rd2gap;
	char orientation;

	for ( t = 0; t < read_c; t++ )
	{
		readCounter++;
		rd1gap = rd2gap = 0;
		ctgId = ctgIdArray[t];

		if ( outfp1 && t % 2 == 1 ) //make sure this is read2 in a pair
		{
			if ( ctgIdArray[t] < 1 && ctgIdArray[t - 1] > 0 )
			{
				getReadIngap ( t, insSizeArr[t], outfp1, 0 );
				rd2gap = 1;
			}
			else if ( ctgIdArray[t] > 0 && ctgIdArray[t - 1] < 1 )
			{
				getReadIngap ( t - 1, insSizeArr[t - 1], outfp1, 1 );
				rd1gap = 1;
			}

		}

		if ( ctgId < 1 )
		{
			continue;
		}

		mapCounter++;
		gzprintf ( outfp, "%lld\t%u\t%d\n", readCounter, ctgIdArray[t], posArray[t] );

		if ( t % 2 == 0 )
		{
			continue;
		}

		// reads are not located by pe info but across edges
		if ( outfp1 && footprint[t - 1] && !rd1gap )
		{
			if ( ctgIdArray[t - 1] < 1 )
			{
				locate1read ( t - 1 );
			}

			output1read_gz ( t - 1, outfp1 ); //read1 in gap.
		}

		if ( outfp1 && footprint[t] && !rd2gap )
		{
			if ( ctgIdArray[t] < 1 )
			{
				locate1read ( t );
			}

			output1read_gz ( t, outfp1 ); //read2 in gap.
		}
	}
}


/*************************************************
Function:
    basicContigInfo
Description:
    Loads contig index and length infromation.
Input:
    1. infile:      the graph file prefix
Output:
    None.
Return:
    None.
*************************************************/
void basicContigInfo ( char * infile )
{
	char name[256], lldne[1024];
	FILE * fp;
	int length, bal_ed, num_all, num_long, index;
	sprintf ( name, "%s.ContigIndex", infile );
	fp = ckopen ( name, "r" );
	fgets ( lldne, sizeof ( lldne ), fp );
	sscanf ( lldne + 8, "%d %d", &num_all, &num_long );
	printf ( "%d edge(s) in the graph.\n", num_all );
	num_ctg = num_all;
	contig_array = ( CONTIG * ) ckalloc ( ( num_all + 1 ) * sizeof ( CONTIG ) );
	fgets ( lldne, sizeof ( lldne ), fp );
	num_long = 0;

	while ( fgets ( lldne, sizeof ( lldne ), fp ) != NULL )
	{
		sscanf ( lldne, "%d %d %d", &index, &length, &bal_ed );
		contig_array[++num_long].length = length;
		contig_array[num_long].bal_edge = bal_ed + 1;

		if ( index != num_long )
		{
			printf ( "BasicContigInfo: %d vs %d.\n", index, num_long );
		}

		if ( bal_ed == 0 )
		{
			continue;
		}

		contig_array[++num_long].length = length;
		contig_array[num_long].bal_edge = -bal_ed + 1;
	}

	fclose ( fp );
}


/*************************************************
Function:
    prlRead2Ctg
Description:
    Maps reads to contigs one by one.
Input:
    1. libfile:     the reads config file
    2. outfile:     the graph file prefix
Output:
    None.
Return:
    None.
*************************************************/
void prlRead2Ctg ( char * libfile, char * outfile )
{
	long long i;
	char * src_name, *next_name, name[256];
	FILE * fo2;
	gzFile * fo, *outfp1 = NULL;
	int maxReadNum, libNo, prevLibNo, insSize = 0;
	boolean flag, pairs = 1;
	pthread_t threads[thrd_num];
	unsigned char thrdSignal[thrd_num + 1];
	PARAMETER paras[thrd_num];
	//init
	maxReadLen = 0;
	maxNameLen = 256;
	scan_libInfo ( libfile );
	alloc_pe_mem ( num_libs );

	if ( !maxReadLen )
	{
		maxReadLen = 100;
	}

	printf ( "In file: %s, max seq len %d, max name len %d\n", libfile, maxReadLen, maxNameLen );

	if ( maxReadLen > maxReadLen4all )
	{
		maxReadLen4all = maxReadLen;
	}

	src_name = ( char * ) ckalloc ( ( maxNameLen + 1 ) * sizeof ( char ) );
	next_name = ( char * ) ckalloc ( ( maxNameLen + 1 ) * sizeof ( char ) );
	kmerBuffer = ( Kmer * ) ckalloc ( buffer_size * sizeof ( Kmer ) );
	hashBanBuffer = ( ubyte8 * ) ckalloc ( buffer_size * sizeof ( ubyte8 ) );
	nodeBuffer = ( kmer_t ** ) ckalloc ( buffer_size * sizeof ( kmer_t * ) );
	smallerBuffer = ( boolean * ) ckalloc ( buffer_size * sizeof ( boolean ) );
	maxReadNum = buffer_size / ( maxReadLen - overlaplen + 1 );
	maxReadNum = maxReadNum % 2 == 0 ? maxReadNum : maxReadNum - 1; //make sure paired reads are processed at the same batch
	seqBuffer = ( char ** ) ckalloc ( maxReadNum * sizeof ( char * ) );
	lenBuffer = ( int * ) ckalloc ( maxReadNum * sizeof ( int ) );
	indexArray = ( unsigned int * ) ckalloc ( ( maxReadNum + 1 ) * sizeof ( unsigned int ) );
	ctgIdArray = ( unsigned int * ) ckalloc ( ( maxReadNum + 1 ) * sizeof ( unsigned int ) );
	posArray = ( int * ) ckalloc ( ( maxReadNum + 1 ) * sizeof ( int ) );
	orienArray = ( char * ) ckalloc ( ( maxReadNum + 1 ) * sizeof ( char ) );
	footprint = ( char * ) ckalloc ( ( maxReadNum + 1 ) * sizeof ( char ) );
	insSizeArray = ( int * ) ckalloc ( ( maxReadNum + 1 ) * sizeof ( int ) );
	read_name = ( char ** ) ckalloc ( maxReadNum * sizeof ( char * ) );

	if ( gLineLen < maxReadLen )
	{
		gStr = ( char * ) ckalloc ( ( maxReadLen + 1 ) * sizeof ( char ) );
	}

	for ( i = 0; i < maxReadNum; i++ )
		{ read_name[i] = ( char * ) ckalloc ( ( maxNameLen + 1 ) * sizeof ( char ) ); }

	for ( i = 0; i < maxReadNum; i++ )
	{
		seqBuffer[i] = ( char * ) ckalloc ( maxReadLen * sizeof ( char ) );
	}

	rcSeq = ( char ** ) ckalloc ( ( thrd_num + 1 ) * sizeof ( char * ) );
	thrdSignal[0] = 0;

	if ( 1 )
	{
		for ( i = 0; i < thrd_num; i++ )
		{
			rcSeq[i + 1] = ( char * ) ckalloc ( maxReadLen * sizeof ( char ) );
			thrdSignal[i + 1] = 0;
			paras[i].threadID = i;
			paras[i].mainSignal = &thrdSignal[0];
			paras[i].selfSignal = &thrdSignal[i + 1];
		}

		creatThrds ( threads, paras );
	}

	if ( !contig_array )
	{
		basicContigInfo ( outfile );
	}

	sprintf ( name, "%s.readInGap.gz", outfile );
	outfp1 = gzopen ( name, "wb" );

	sprintf ( name, "%s.readOnContig.gz", outfile );
	fo = gzopen ( name, "w" );


	gzprintf ( fo, "read\tcontig\tpos\n" );
	readCounter = mapCounter = readsInGap = 0;
	kmer_c = n_solexa = read_c = i = libNo = readNumBack = gradsCounter = 0;
	prevLibNo = -1;
	int type = 0;       //decide whether the PE reads is good or bad
	printf ("Read mapping starting with overlaplen=%d\n",overlaplen);

	while ( ( flag = read1seqInLib ( seqBuffer[read_c], read_name[read_c], & ( lenBuffer[read_c] ), &libNo, pairs, 0, &type ) ) != 0 )
	{
		if ( type == -1 ) //if the reads is bad, go back.
		{
			i--;

			if ( lenBuffer[read_c - 1] >= overlaplen + 1 )
			{
				kmer_c -= lenBuffer[read_c - 1] - overlaplen + 1;
			}

			read_c--;
			n_solexa -= 2;
			continue;
		}

		if ( libNo != prevLibNo )
		{
			prevLibNo = libNo;
			insSize = lib_array[libNo].avg_ins;
			ALIGNLEN = lib_array[libNo].map_len;

			if ( insSize > 1000 )
			{
				ALIGNLEN = ALIGNLEN < 35 ? 35 : ALIGNLEN;
			}
			else
			{
				ALIGNLEN = ALIGNLEN < 32 ? 32 : ALIGNLEN;
			}

			printf ( "Current insert size is %d, map_len is %d.\n", insSize, ALIGNLEN );
		}

		insSizeArray[read_c] = insSize;

		/*XXX: bj mod (DELETED)
		if ( insSize > 1000 )
		{
			ALIGNLEN = ALIGNLEN < ( lenBuffer[read_c] / 2 + 1 ) ? ( lenBuffer[read_c] / 2 + 1 ) : ALIGNLEN;
		}*/

		if ( ( ++i ) % 100000000 == 0 )
		{
			printf ( "--- %lldth reads.\n", i );
		}

		indexArray[read_c] = kmer_c;

		if ( lenBuffer[read_c] >= overlaplen + 1 )
		{
			kmer_c += lenBuffer[read_c] - overlaplen + 1;
		}

		read_c++;

		if ( read_c == maxReadNum )
		{
			indexArray[read_c] = kmer_c;
			sendWorkSignal ( 2, thrdSignal ); //chopKmer4read
			sendWorkSignal ( 1, thrdSignal ); //searchKmer
			sendWorkSignal ( 3, thrdSignal ); //parse1read
			recordAlldgn ( fo, insSizeArray, outfp1 );
			kmer_c = 0;
			read_c = 0;
		}
	}

	if ( read_c )
	{
		indexArray[read_c] = kmer_c;
		sendWorkSignal ( 2, thrdSignal ); //chopKmer4read
		sendWorkSignal ( 1, thrdSignal ); //searchKmer
		sendWorkSignal ( 3, thrdSignal ); //parse1read
		recordAlldgn ( fo, insSizeArray, outfp1 );
		printf ( "\nTotal reads         %lld\n", readCounter );
		printf ( "Reads in gaps       %lld\n", readsInGap );
		printf ( "Ratio               %.1f%%\n", ( float ) readsInGap / readCounter * 100 );
	}

	printf ( "Reads on contigs    %lld\n", mapCounter );
	printf ( "Ratio               %.1f%%\n", ( float ) mapCounter / readCounter * 100 );
	sendWorkSignal ( 5, thrdSignal ); //stop threads
	thread_wait ( threads );
	gzclose ( fo );
	sprintf ( name, "%s.peGrads", outfile );
	fo2 = ckopen ( name, "w" );
	fprintf ( fo2, "grads&num: %d\t%lld\t%d\n", gradsCounter, n_solexa, maxReadLen4all );

	if ( pairs )
	{
		if ( gradsCounter )
			{ printf ( "%d pe insert size, the largest boundary is %lld.\n\n", gradsCounter, pes[gradsCounter - 1].PE_bound ); }
		else
		{
			printf ( "No paired reads found.\n" );
		}

		for ( i = 0; i < gradsCounter; i++ )
		{
			fprintf ( fo2, "%d\t%lld\t%d\t%d\n", pes[i].insertS, pes[i].PE_bound, pes[i].rank, pes[i].pair_num_cut );
		}

		fclose ( fo2 );
	}

	gzclose ( outfp1 );


	free_pe_mem ();
	free_libs ();

	if ( 1 )        // multi-threads
	{
		for ( i = 0; i < thrd_num; i++ )
		{
			free ( ( void * ) rcSeq[i + 1] );
		}
	}

	free ( ( void * ) rcSeq );

	for ( i = 0; i < maxReadNum; i++ )
	{
		free ( ( void * ) seqBuffer[i] );
	}

	free ( ( void * ) seqBuffer );
	free ( ( void * ) lenBuffer );
	free ( ( void * ) indexArray );

	for ( i = 0; i < maxReadNum; i++ )
		{ free ( ( void * ) read_name[i] ); }

	free ( ( void * ) read_name );
	free ( ( void * ) kmerBuffer );
	free ( ( void * ) smallerBuffer );
	free ( ( void * ) hashBanBuffer );
	free ( ( void * ) nodeBuffer );
	free ( ( void * ) ctgIdArray );
	free ( ( void * ) posArray );
	free ( ( void * ) orienArray );
	free ( ( void * ) footprint );
	free ( ( void * ) insSizeArray );
	free ( ( void * ) src_name );
	free ( ( void * ) next_name );

	if ( gLineLen < maxReadLen )
	{
		free ( ( void * ) gStr );
		gStr = NULL;
	}

	if ( contig_array )
	{
		free ( ( void * ) contig_array );
		contig_array = NULL;
	}
}

static void thread_wait ( pthread_t * threads )
{
	int i;

	for ( i = 0; i < thrd_num; i++ )
		if ( threads[i] != 0 )
		{
			pthread_join ( threads[i], NULL );
		}
}
